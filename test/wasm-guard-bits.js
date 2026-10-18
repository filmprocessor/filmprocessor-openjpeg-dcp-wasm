import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

// Guard bits are fixed per resolution: 1 for a 2K codestream, 2 for 4K. A
// Bv2.1 verifier rejects either count at the other resolution. This encodes
// through the locally built Wasm binary, so a stale rebuild fails here rather
// than only in a packaged DCP.

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

const WIDTH = 256;
const HEIGHT = 128;

function encode(profileConfig) {
  const samples = new Int32Array(WIDTH * HEIGHT * 3);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const i = (y * WIDTH + x) * 3;
      samples[i] = (x * 4095) / (WIDTH - 1);
      samples[i + 1] = (y * 4095) / (HEIGHT - 1);
      samples[i + 2] = 2048;
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

// Walk the main header's marker segments and return the guard-bit count of
// the QCD and of every QCC (Sqcd/Sqcc high three bits).
function mainHeaderGuardBits(codestream) {
  const view = new DataView(codestream.buffer, codestream.byteOffset, codestream.byteLength);
  assert.equal(view.getUint16(0), 0xff4f, "codestream does not start with SOC");
  const guardBits = [];
  let offset = 2;
  while (offset + 4 <= codestream.byteLength) {
    const marker = view.getUint16(offset);
    if (marker === 0xff90) break;
    const length = view.getUint16(offset + 2);
    if (marker === 0xff5c) {
      guardBits.push(codestream[offset + 4] >> 5);
    } else if (marker === 0xff5d) {
      // Cqcc is one byte when there are fewer than 257 components.
      guardBits.push(codestream[offset + 5] >> 5);
    }
    offset += 2 + length;
  }
  return guardBits;
}

for (const fps of [24, 48]) {
  test(`2K cinema profile at ${fps} fps writes 1 guard bit`, () => {
    const guardBits = mainHeaderGuardBits(encode({ rsiz: 0x0003, fps }));
    assert.ok(guardBits.length > 0, "no QCD marker in the main header");
    assert.deepEqual(new Set(guardBits), new Set([1]));
  });
}

test("4K cinema profile writes 2 guard bits", () => {
  const guardBits = mainHeaderGuardBits(encode({ rsiz: 0x0004, fps: 24 }));
  assert.ok(guardBits.length > 0, "no QCD marker in the main header");
  assert.deepEqual(new Set(guardBits), new Set([2]));
});
