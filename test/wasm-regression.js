import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

// openjpeg_decode_j2k_reduced() sheds a codestream's top resolution levels, so
// a consumer can decode a 4K codestream at a lower resolution.
// Full-resolution decoding must remain compatible. This runs the local build.

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const WASM_DIR = path.join(__dirname, "..", "dist");

const resolveOpenJPEGCinemaMode = ({rsiz, fps}) => rsiz === 4 ? 3 : fps > 30 ? 2 : 1;
const getOpenJPEGMaxCodestreamSize = (fps) => Math.floor(250000000 / 8 / fps * 0.95);
const getOpenJPEGMaxComponentSize = (fps) => Math.floor(200000000 / 8 / fps * 0.95);
const { default: createOpenJPEGModule } = await import(
  pathToFileURL(path.join(WASM_DIR, "openjpeg-dcp.js")).href
);

const Module = await createOpenJPEGModule({
  wasmBinary: readFileSync(path.join(WASM_DIR, "openjpeg-dcp.wasm")),
  print() {},
  printErr() {},
});

// Odd sizes, so the halving has to round up.
const WIDTH = 509;
const HEIGHT = 255;

function encode(profileConfig) {
  const samples = new Int32Array(WIDTH * HEIGHT * 3);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const i = (y * WIDTH + x) * 3;
      samples[i] = Math.round((x * 4095) / (WIDTH - 1));
      samples[i + 1] = Math.round((y * 4095) / (HEIGHT - 1));
      samples[i + 2] = 1024 + Math.round(1024 * Math.sin(x / 23) * Math.cos(y / 17));
    }
  }
  const inputPtr = Module._malloc(samples.byteLength);
  const sizePtr = Module._malloc(4);
  Module.HEAP32.set(samples, inputPtr >> 2);
  const outputPtr = Module.ccall(
    "openjpeg_encode_xyz",
    "number",
    ["number", "number", "number", "number", "number", "number", "number"],
    [
      inputPtr,
      WIDTH,
      HEIGHT,
      resolveOpenJPEGCinemaMode(profileConfig),
      getOpenJPEGMaxCodestreamSize(profileConfig.fps),
      getOpenJPEGMaxComponentSize(profileConfig.fps),
      sizePtr,
    ],
  );
  const size = Module.getValue(sizePtr, "i32");
  Module._free(inputPtr);
  Module._free(sizePtr);
  assert.ok(outputPtr && size > 0, "encode returned no codestream");
  const codestream = Module.HEAPU8.slice(outputPtr, outputPtr + size);
  Module._openjpeg_free_buffer(outputPtr);
  return codestream;
}

// reduce === null calls the full-resolution export.
function decode(codestream, reduce) {
  const inputPtr = Module._malloc(codestream.length);
  Module.HEAPU8.set(codestream, inputPtr);
  const out = Module._malloc(16);
  const resultPtr = reduce === null
    ? Module._openjpeg_decode_j2k(inputPtr, codestream.length, out, out + 4, out + 8, out + 12)
    : Module._openjpeg_decode_j2k_reduced(inputPtr, codestream.length, reduce, out, out + 4, out + 8, out + 12);
  const width = Module.getValue(out, "i32");
  const height = Module.getValue(out + 4, "i32");
  const numComps = Module.getValue(out + 8, "i32");
  const bitDepth = Module.getValue(out + 12, "i32");
  const samples = resultPtr
    ? new Int32Array(Module.HEAP32.buffer.slice(resultPtr, resultPtr + width * height * numComps * 4))
    : null;
  if (resultPtr) Module._openjpeg_free_buffer(resultPtr);
  Module._free(inputPtr);
  Module._free(out);
  return { resultPtr, width, height, numComps, bitDepth, samples };
}

// Mean absolute difference between a reduced decode and the 2x2 average of
// the full-resolution decode, in 12-bit code values.
function meanDifferenceFromBoxAverage(full, half) {
  let total = 0;
  let count = 0;
  for (let y = 0; y < half.height; y++) {
    for (let x = 0; x < half.width; x++) {
      for (let c = 0; c < 3; c++) {
        let sum = 0;
        let n = 0;
        for (let dy = 0; dy < 2; dy++) {
          for (let dx = 0; dx < 2; dx++) {
            const fx = 2 * x + dx;
            const fy = 2 * y + dy;
            if (fx >= full.width || fy >= full.height) continue;
            sum += full.samples[(fy * full.width + fx) * 3 + c];
            n++;
          }
        }
        total += Math.abs(half.samples[(y * half.width + x) * 3 + c] - sum / n);
        count++;
      }
    }
  }
  return total / count;
}

for (const [label, profile, levels] of [
  ["4K", { rsiz: 0x0004, fps: 24 }, 7],
  ["2K", { rsiz: 0x0003, fps: 24 }, 6],
]) {
  const codestream = encode(profile);

  test(`${label}: reducing by nothing is the full-resolution decode, sample for sample`, () => {
    const full = decode(codestream, null);
    const same = decode(codestream, 0);
    assert.deepEqual([full.width, full.height, full.numComps, full.bitDepth], [WIDTH, HEIGHT, 3, 12]);
    assert.deepEqual([same.width, same.height, same.numComps, same.bitDepth], [WIDTH, HEIGHT, 3, 12]);
    assert.deepEqual(same.samples, full.samples);
  });

  test(`${label}: one level shed halves each side, rounding up, and keeps the picture`, () => {
    const full = decode(codestream, null);
    const half = decode(codestream, 1);
    assert.deepEqual(
      [half.width, half.height, half.numComps, half.bitDepth],
      [Math.ceil(WIDTH / 2), Math.ceil(HEIGHT / 2), 3, 12],
    );
    // The wavelet's low band is not a box filter, but it is the same picture:
    // well under 1% of the 12-bit range from the box-averaged full decode.
    assert.ok(meanDifferenceFromBoxAverage(full, half) < 20);
  });

  test(`${label}: a reduction that leaves no resolution level is refused`, () => {
    const smallest = decode(codestream, levels - 1);
    assert.ok(smallest.resultPtr, "the lowest level still decodes");
    assert.equal(smallest.width, Math.ceil(WIDTH / 2 ** (levels - 1)));
    const none = decode(codestream, levels);
    assert.equal(none.resultPtr, 0);
    assert.equal(none.width, 0);
    assert.equal(decode(codestream, -1).resultPtr, 0);
  });
}
