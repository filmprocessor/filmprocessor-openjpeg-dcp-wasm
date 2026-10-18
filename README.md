# FilmProcessor OpenJPEG DCP Wasm

Emscripten build script and C wrapper for compiling OpenJPEG into a
DCP-oriented WebAssembly module.

This repository intentionally ships source and build instructions only. It does
not include FilmProcessor's production encoder wrapper, DCP authoring pipeline,
MXF writer, color pipeline, cloud processing, server APIs, UI, or prebuilt Wasm
binary.

## Contents

- `src/openjpeg_dcp_wrapper.c` - memory-stream wrapper around OpenJPEG encode
  and decode functions.
- `scripts/build_openjpeg_wasm.sh` - reproducible build entrypoint for
  OpenJPEG 2.5.0 using Emscripten.

## Build

Install and activate Emscripten, then run:

```bash
EMSDK_DIR=/path/to/emsdk ./scripts/build_openjpeg_wasm.sh
```

The script writes generated artifacts to `dist/`. Run `npm test` for package
checks and `npm run test:wasm` after building for encode/decode regressions.

## 0.2.0 API update

The C wrapper is synchronized with FilmProcessor’s 17 October 2026 release.
Rebuild the JavaScript glue and Wasm together.

`openjpeg_encode_xyz(input, width, height, cinemaProfile, maxCodestreamSize,
maxComponentSize, outputSize)` now takes **seven arguments**: insert the separate
component budget before the output-size pointer. Update `ccall`/`cwrap` signatures
as well as argument values; the old six-argument call is incompatible.
Budgets are bytes, and zero disables the corresponding explicit cap. Callers
remain responsible for frame-rate-specific DCI budgets and overhead margins.

Profile 1 selects 2K at 24/25/30 fps, profile 2 selects 2K at 48/50/60 fps,
and profile 3 selects 4K. Guard bits are set to 1 for 2K and 2 for 4K.

`openjpeg_decode_j2k_reduced(input, size, reduce, widthOut, heightOut,
componentsOut, bitDepthOut)` discards the requested highest resolution levels.
Zero preserves full resolution; one halves each dimension, rounding up. The
existing full-resolution decode function remains available. Free returned
buffers with `openjpeg_free_buffer`.

## What This Is Not

This is not a full DCP authoring library. It does not generate MXF, CPL, PKL,
ASSETMAP, VOLINDEX, KDM, DKDM, or encrypted AS-DCP packages.

## Third-Party Notices

OpenJPEG is developed by the OpenJPEG project and is licensed under the
2-clause BSD License. See `NOTICE`.

## License

The FilmProcessor wrapper and build script are licensed under BSD-2-Clause.
OpenJPEG itself remains under its upstream license.
