/**
 * OpenJPEG DCP Wrapper
 * JPEG2000 encoding & decoding for Digital Cinema Packages (SMPTE ST 429-4)
 *
 * Wraps OpenJPEG 2.5.0 to provide cinema-compliant JPEG2000 encoding
 * with MCT (Multi-Component Transform / ICT) enabled as required by DCP,
 * plus J2K codestream decoding for DCP playback.
 *
 * Exported functions:
 *   openjpeg_encode_xyz()  - Encode XYZ image data to JPEG2000 codestream
 *   openjpeg_decode_j2k()  - Decode JPEG2000 codestream to XYZ image data
 *   openjpeg_free_buffer() - Free buffer allocated by encoder/decoder
 *
 * Build: See build_openjpeg_wasm.sh
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <openjpeg.h>

/* Cinema profile constants (match JS-side enum) */
#define CINEMA_2K_24 1
#define CINEMA_2K_48 2
#define CINEMA_4K_24 3

/* DCP encoding constants */
#define DCP_BIT_DEPTH    12
#define DCP_NUM_COMPS    3
#define DCP_DWT_2K       5
#define DCP_DWT_4K       6

/* Memory stream state for OpenJPEG output */
typedef struct {
    uint8_t *data;
    OPJ_SIZE_T size;
    OPJ_SIZE_T offset;
    OPJ_SIZE_T capacity;
} mem_stream_t;

/**
 * Error callback for OpenJPEG
 */
static void error_callback(const char *msg, void *client_data) {
    (void)client_data;
    fprintf(stderr, "[OpenJPEG ERROR] %s", msg);
}

/**
 * Warning callback for OpenJPEG
 */
static void warning_callback(const char *msg, void *client_data) {
    (void)client_data;
    fprintf(stderr, "[OpenJPEG WARN] %s", msg);
}

/**
 * Info callback for OpenJPEG
 */
static void info_callback(const char *msg, void *client_data) {
    (void)client_data;
    fprintf(stdout, "[OpenJPEG] %s", msg);
}

/**
 * Stream write callback for memory-backed output
 */
static int mem_write_count = 0;
static OPJ_SIZE_T mem_write(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    OPJ_SIZE_T remaining = s->capacity - s->offset;
    OPJ_SIZE_T toWrite = (p_nb_bytes < remaining) ? p_nb_bytes : remaining;
    if (toWrite > 0) {
        memcpy(s->data + s->offset, p_buffer, toWrite);
        s->offset += toWrite;
        if (s->offset > s->size) {
            s->size = s->offset;
        }
    }
    mem_write_count++;
    /* Log every 100th write to avoid flooding */
    if (mem_write_count <= 5 || mem_write_count % 100 == 0) {
        fprintf(stderr, "[DCP Wrapper] mem_write #%d: %zu bytes (total=%zu)\n",
                mem_write_count, p_nb_bytes, s->size);
    }
    return toWrite;
}

/**
 * Stream seek callback for memory-backed output
 */
static OPJ_BOOL mem_seek(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    if (p_nb_bytes < 0 || (OPJ_SIZE_T)p_nb_bytes > s->capacity) {
        return OPJ_FALSE;
    }
    s->offset = (OPJ_SIZE_T)p_nb_bytes;
    return OPJ_TRUE;
}

/**
 * Stream skip callback for memory-backed output
 */
static OPJ_OFF_T mem_skip(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    OPJ_SIZE_T newOffset = s->offset + (OPJ_SIZE_T)p_nb_bytes;
    if (newOffset > s->capacity) {
        newOffset = s->capacity;
    }
    OPJ_OFF_T skipped = (OPJ_OFF_T)(newOffset - s->offset);
    s->offset = newOffset;
    return skipped;
}

/**
 * Stream free callback (no-op, buffer is managed externally)
 */
static void mem_free(void *p_user_data) {
    (void)p_user_data;
}

/**
 * Encode XYZ image data to a DCP-compliant JPEG2000 codestream.
 *
 * @param xyzData       Pointer to interleaved XYZ pixel data (int32 per sample,
 *                      stored as X0,Y0,Z0, X1,Y1,Z1, ... in 12-bit range 0-4095)
 * @param width         Image width in pixels
 * @param height        Image height in pixels
 * @param cinemaProfile Cinema profile: 1=2K@24, 2=2K@48, 3=4K@24
 * @param maxCodestreamSize Maximum output size in bytes (DCI bitrate limit)
 * @param outputSize    Pointer to int that receives the output codestream size
 *
 * @return Pointer to the JPEG2000 codestream buffer (caller must free with
 *         openjpeg_free_buffer), or NULL on failure.
 */
uint8_t* openjpeg_encode_xyz(
    int32_t* xyzData,
    int width,
    int height,
    int cinemaProfile,
    int maxCodestreamSize,
    int* outputSize
) {
    opj_cparameters_t parameters;
    opj_codec_t *codec = NULL;
    opj_image_t *image = NULL;
    opj_stream_t *stream = NULL;
    uint8_t *outputBuffer = NULL;
    OPJ_BOOL success = OPJ_FALSE;

    *outputSize = 0;
    mem_write_count = 0;

    /* Set default encoder parameters */
    opj_set_default_encoder_parameters(&parameters);

    /* Match the reference D-Cinema codestream comment marker rather than
     * OpenJPEG's verbose default string. This keeps the COM segment aligned
     * with known-good packages while still using OpenJPEG for the actual
     * codestream generation. */
    parameters.cp_comment = "libdcp";

    /* Configure DCP-compliant JPEG2000 encoding parameters explicitly.
     * IMPORTANT: We do NOT use the deprecated cp_cinema field (OPJ_CINEMA2K_24 etc.)
     * because OpenJPEG 2.5.0's internal cinema setup function can conflict with
     * explicit max_cs_size, causing rate-control non-convergence.
     * Instead, we set all required parameters individually per SMPTE ST 429-4. */

    /* Profile-specific settings */
    int numDwtLevels;
    switch (cinemaProfile) {
        case CINEMA_2K_24:
        case CINEMA_2K_48:
            parameters.rsiz = OPJ_PROFILE_CINEMA_2K;
            numDwtLevels = DCP_DWT_2K;  /* 5 DWT levels */
            break;
        case CINEMA_4K_24:
            parameters.rsiz = OPJ_PROFILE_CINEMA_4K;
            numDwtLevels = DCP_DWT_4K;  /* 6 DWT levels */
            break;
        default:
            parameters.rsiz = OPJ_PROFILE_CINEMA_2K;
            numDwtLevels = DCP_DWT_2K;
            break;
    }

    parameters.numresolution = numDwtLevels + 1;

    /* Irreversible compression (9-7 DWT + ICT) — required by DCP */
    parameters.irreversible = 1;

    /* Multi-Component Transform (ICT) — required by SMPTE ST 429-4 */
    parameters.tcp_mct = 1;

    /* Single quality layer with rate control via max_cs_size */
    parameters.tcp_numlayers = 1;
    parameters.tcp_rates[0] = 0;  /* Let max_cs_size control the rate */
    parameters.cp_disto_alloc = 1;

    /* Single tile spanning entire image */
    parameters.cp_tx0 = 0;
    parameters.cp_ty0 = 0;
    parameters.cp_tdx = width;
    parameters.cp_tdy = height;

    /* Progression order: CPRL (Component-Position-Resolution-Layer) per DCI */
    parameters.prog_order = OPJ_CPRL;

    /* Code block size: 32x32 (DCP requirement) */
    parameters.cblockw_init = 32;
    parameters.cblockh_init = 32;

    /* Precincts: 256x256 for all levels except lowest which is 128x128 */
    parameters.csty |= 0x01;  /* Enable precincts */
    parameters.res_spec = parameters.numresolution;
    for (int i = 0; i < parameters.numresolution; i++) {
        if (i == 0) {
            parameters.prcw_init[i] = 128;
            parameters.prch_init[i] = 128;
        } else {
            parameters.prcw_init[i] = 256;
            parameters.prch_init[i] = 256;
        }
    }

    /* Rate control: max codestream size from DCI bitrate limit */
    if (maxCodestreamSize > 0) {
        parameters.max_cs_size = maxCodestreamSize;
        parameters.max_comp_size = maxCodestreamSize;  /* Per-component limit */
    }

    /* Create image component parameters */
    opj_image_cmptparm_t cmptparm[DCP_NUM_COMPS];
    memset(cmptparm, 0, DCP_NUM_COMPS * sizeof(opj_image_cmptparm_t));

    for (int i = 0; i < DCP_NUM_COMPS; i++) {
        cmptparm[i].prec = DCP_BIT_DEPTH;
        cmptparm[i].bpp = DCP_BIT_DEPTH;
        cmptparm[i].sgnd = 0;
        cmptparm[i].dx = 1;
        cmptparm[i].dy = 1;
        cmptparm[i].w = (OPJ_UINT32)width;
        cmptparm[i].h = (OPJ_UINT32)height;
    }

    /* Create the image (use CLRSPC_SRGB; OpenJPEG treats it as 3-component) */
    image = opj_image_create(DCP_NUM_COMPS, cmptparm, OPJ_CLRSPC_SRGB);
    if (!image) {
        return NULL;
    }

    /* Set image geometry */
    image->x0 = 0;
    image->y0 = 0;
    image->x1 = (OPJ_UINT32)width;
    image->y1 = (OPJ_UINT32)height;

    /* Copy XYZ data into image components */
    int pixelCount = width * height;
    for (int i = 0; i < pixelCount; i++) {
        image->comps[0].data[i] = xyzData[i * 3];     /* X */
        image->comps[1].data[i] = xyzData[i * 3 + 1]; /* Y */
        image->comps[2].data[i] = xyzData[i * 3 + 2]; /* Z */
    }

    /* Create J2K codec (raw codestream, not JP2 container) */
    codec = opj_create_compress(OPJ_CODEC_J2K);
    if (!codec) {
        opj_image_destroy(image);
        return NULL;
    }

    /* Set callbacks */
    opj_set_error_handler(codec, error_callback, NULL);
    opj_set_warning_handler(codec, warning_callback, NULL);
    opj_set_info_handler(codec, info_callback, NULL);

    /* Setup encoder with parameters */
    if (!opj_setup_encoder(codec, &parameters, image)) {
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return NULL;
    }

    /* DCI compliance: Set guard bits to 1 (SMPTE ST 429-4 requirement).
     * OpenJPEG defaults to 2 guard bits; DCI requires exactly 1 for 2K/4K.
     * Must be called AFTER opj_setup_encoder, BEFORE opj_start_compress. */
    const char* extra_options[] = {"GUARD_BITS=1", NULL};
    if (!opj_encoder_set_extra_options(codec, extra_options)) {
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return NULL;
    }

    /* Create memory stream for output */
    int bufferSize = maxCodestreamSize + 65536;
    outputBuffer = (uint8_t*)malloc(bufferSize);
    if (!outputBuffer) {
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return NULL;
    }

    stream = opj_stream_create(bufferSize, OPJ_FALSE);
    if (!stream) {
        free(outputBuffer);
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return NULL;
    }

    /* Initialize memory stream state */
    mem_stream_t ms;
    ms.data = outputBuffer;
    ms.size = 0;
    ms.offset = 0;
    ms.capacity = bufferSize;

    opj_stream_set_user_data(stream, &ms, mem_free);
    opj_stream_set_user_data_length(stream, bufferSize);
    opj_stream_set_write_function(stream, mem_write);
    opj_stream_set_seek_function(stream, mem_seek);
    opj_stream_set_skip_function(stream, mem_skip);

    /* Encode with progress logging */
    fprintf(stderr, "[DCP Wrapper] Starting compress (%dx%d, profile=%d, maxCS=%d)...\n",
            width, height, cinemaProfile, maxCodestreamSize);

    success = opj_start_compress(codec, image, stream);
    fprintf(stderr, "[DCP Wrapper] opj_start_compress: %s\n", success ? "OK" : "FAILED");

    if (success) {
        fprintf(stderr, "[DCP Wrapper] Calling opj_encode...\n");
        success = opj_encode(codec, stream);
        fprintf(stderr, "[DCP Wrapper] opj_encode: %s (stream size=%zu)\n",
                success ? "OK" : "FAILED", ms.size);
    }

    if (success) {
        fprintf(stderr, "[DCP Wrapper] Calling opj_end_compress...\n");
        success = opj_end_compress(codec, stream);
        fprintf(stderr, "[DCP Wrapper] opj_end_compress: %s (final size=%zu)\n",
                success ? "OK" : "FAILED", ms.size);
    }

    /* Cleanup codec and image */
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    opj_image_destroy(image);

    if (!success) {
        free(outputBuffer);
        return NULL;
    }

    /* Set output size */
    *outputSize = (int)ms.size;

    return outputBuffer;
}

/**
 * Free a buffer previously returned by openjpeg_encode_xyz() or openjpeg_decode_j2k().
 *
 * @param ptr Pointer to buffer to free
 */
void openjpeg_free_buffer(uint8_t* ptr) {
    if (ptr) {
        free(ptr);
    }
}

/* ─── JPEG2000 Decoding ─────────────────────────────────────────────────── */

/**
 * Stream read callback for memory-backed input
 */
static OPJ_SIZE_T mem_read(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    OPJ_SIZE_T remaining = s->size - s->offset;
    if (remaining == 0) return (OPJ_SIZE_T)-1; /* EOF */
    OPJ_SIZE_T toRead = (p_nb_bytes < remaining) ? p_nb_bytes : remaining;
    memcpy(p_buffer, s->data + s->offset, toRead);
    s->offset += toRead;
    return toRead;
}

/**
 * Stream seek callback for memory-backed input
 */
static OPJ_BOOL mem_read_seek(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    if (p_nb_bytes < 0 || (OPJ_SIZE_T)p_nb_bytes > s->size) {
        return OPJ_FALSE;
    }
    s->offset = (OPJ_SIZE_T)p_nb_bytes;
    return OPJ_TRUE;
}

/**
 * Stream skip callback for memory-backed input
 */
static OPJ_OFF_T mem_read_skip(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    mem_stream_t *s = (mem_stream_t*)p_user_data;
    if (p_nb_bytes < 0) return -1;
    OPJ_SIZE_T newOffset = s->offset + (OPJ_SIZE_T)p_nb_bytes;
    if (newOffset > s->size) {
        newOffset = s->size;
    }
    OPJ_OFF_T skipped = (OPJ_OFF_T)(newOffset - s->offset);
    s->offset = newOffset;
    return skipped;
}

/**
 * Decode a JPEG2000 codestream (raw J2K, as found in DCP MXF files) to
 * interleaved component data.
 *
 * @param j2kData    Pointer to raw J2K codestream bytes
 * @param j2kSize    Size of the codestream in bytes
 * @param outWidth   Receives decoded image width
 * @param outHeight  Receives decoded image height
 * @param outNumComps Receives number of components (typically 3 for XYZ)
 * @param outBitDepth Receives component bit depth (typically 12 for DCP)
 *
 * @return Pointer to interleaved int32 component data
 *         [C0_0, C1_0, C2_0, C0_1, C1_1, C2_1, ...] where Cn_i is
 *         component n at pixel i. Caller must free with openjpeg_free_buffer().
 *         Returns NULL on failure.
 */
int32_t* openjpeg_decode_j2k(
    uint8_t* j2kData,
    int j2kSize,
    int* outWidth,
    int* outHeight,
    int* outNumComps,
    int* outBitDepth
) {
    opj_dparameters_t parameters;
    opj_codec_t *codec = NULL;
    opj_image_t *image = NULL;
    opj_stream_t *stream = NULL;
    int32_t *outputBuffer = NULL;

    *outWidth = 0;
    *outHeight = 0;
    *outNumComps = 0;
    *outBitDepth = 0;

    if (!j2kData || j2kSize <= 0) {
        fprintf(stderr, "[DCP Decode] Invalid input: data=%p size=%d\n", (void*)j2kData, j2kSize);
        return NULL;
    }

    /* Set default decoder parameters */
    opj_set_default_decoder_parameters(&parameters);
    parameters.decod_format = 0; /* J2K raw codestream */

    /* Create J2K decompressor (raw codestream, not JP2 container) */
    codec = opj_create_decompress(OPJ_CODEC_J2K);
    if (!codec) {
        fprintf(stderr, "[DCP Decode] Failed to create decompressor\n");
        return NULL;
    }

    /* Set callbacks */
    opj_set_error_handler(codec, error_callback, NULL);
    opj_set_warning_handler(codec, warning_callback, NULL);
    opj_set_info_handler(codec, info_callback, NULL);

    /* Setup decoder */
    if (!opj_setup_decoder(codec, &parameters)) {
        fprintf(stderr, "[DCP Decode] Failed to setup decoder\n");
        opj_destroy_codec(codec);
        return NULL;
    }

    /* Create memory-backed input stream with default buffer size */
    stream = opj_stream_default_create(OPJ_TRUE); /* OPJ_TRUE = input stream */
    if (!stream) {
        fprintf(stderr, "[DCP Decode] Failed to create stream\n");
        opj_destroy_codec(codec);
        return NULL;
    }

    mem_stream_t ms;
    ms.data = j2kData;
    ms.size = (OPJ_SIZE_T)j2kSize;
    ms.offset = 0;
    ms.capacity = (OPJ_SIZE_T)j2kSize;

    opj_stream_set_user_data(stream, &ms, mem_free);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)j2kSize);
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_seek_function(stream, mem_read_seek);
    opj_stream_set_skip_function(stream, mem_read_skip);

    /* Read codestream header */
    if (!opj_read_header(stream, codec, &image)) {
        fprintf(stderr, "[DCP Decode] Failed to read J2K header\n");
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return NULL;
    }

    OPJ_UINT32 width  = image->x1 - image->x0;
    OPJ_UINT32 height = image->y1 - image->y0;
    OPJ_UINT32 numComps = image->numcomps;
    OPJ_UINT32 bitDepth = image->comps[0].prec;

    fprintf(stderr, "[DCP Decode] Header: %ux%u, %u components, %u-bit\n",
            width, height, numComps, bitDepth);

    /* Decode the image */
    if (!opj_decode(codec, stream, image)) {
        fprintf(stderr, "[DCP Decode] opj_decode failed\n");
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return NULL;
    }

    if (!opj_end_decompress(codec, stream)) {
        fprintf(stderr, "[DCP Decode] opj_end_decompress failed\n");
        /* Non-fatal: image data may still be valid, continue */
    }

    /* Verify component data is present */
    for (OPJ_UINT32 c = 0; c < numComps; c++) {
        if (!image->comps[c].data) {
            fprintf(stderr, "[DCP Decode] Component %u has no data\n", c);
            opj_image_destroy(image);
            opj_stream_destroy(stream);
            opj_destroy_codec(codec);
            return NULL;
        }
    }

    /* Interleave component data into output buffer:
     * [C0_px0, C1_px0, C2_px0, C0_px1, C1_px1, C2_px1, ...] */
    OPJ_UINT32 pixelCount = width * height;
    outputBuffer = (int32_t*)malloc(pixelCount * numComps * sizeof(int32_t));
    if (!outputBuffer) {
        fprintf(stderr, "[DCP Decode] Failed to allocate output buffer (%u pixels x %u comps)\n",
                pixelCount, numComps);
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return NULL;
    }

    for (OPJ_UINT32 i = 0; i < pixelCount; i++) {
        for (OPJ_UINT32 c = 0; c < numComps; c++) {
            outputBuffer[i * numComps + c] = image->comps[c].data[i];
        }
    }

    /* Set output parameters */
    *outWidth    = (int)width;
    *outHeight   = (int)height;
    *outNumComps = (int)numComps;
    *outBitDepth = (int)bitDepth;

    fprintf(stderr, "[DCP Decode] Success: %ux%u, %u comps, %u-bit (%u pixels)\n",
            width, height, numComps, bitDepth, pixelCount);

    /* Cleanup */
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);

    return outputBuffer;
}
