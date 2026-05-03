import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

test("build script does not contain local absolute development paths", async () => {
  const script = await readFile(new URL("../scripts/build_openjpeg_wasm.sh", import.meta.url), "utf8");
  const localHomePath = ["", "Users", "filmprocessor"].join("/");
  assert.equal(script.includes(localHomePath), false);
  assert.equal(script.includes("EMSDK_DIR"), true);
  assert.match(script, /OPENJPEG_VERSION="2\.5\.0"/);
});

test("C wrapper exposes encode, decode, and free functions", async () => {
  const wrapper = await readFile(new URL("../src/openjpeg_dcp_wrapper.c", import.meta.url), "utf8");
  assert.match(wrapper, /openjpeg_encode_xyz/);
  assert.match(wrapper, /openjpeg_decode_j2k/);
  assert.match(wrapper, /openjpeg_free_buffer/);
});
