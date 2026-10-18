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

test("C wrapper writes 1 guard bit for 2K and 2 for 4K", async () => {
  const wrapper = await readFile(new URL("../src/openjpeg_dcp_wrapper.c", import.meta.url), "utf8");
  assert.match(wrapper, /#define DCP_GUARD_BITS_2K "GUARD_BITS=1"/);
  assert.match(wrapper, /#define DCP_GUARD_BITS_4K "GUARD_BITS=2"/);

  const caseBody = (label) => {
    const start = wrapper.indexOf(`${label}:`);
    assert.notEqual(start, -1, `missing ${label} case`);
    return wrapper.slice(start, wrapper.indexOf("break;", start));
  };
  assert.match(caseBody("case CINEMA_2K_48"), /guardBitsOption = DCP_GUARD_BITS_2K;/);
  assert.match(caseBody("case CINEMA_4K_24"), /guardBitsOption = DCP_GUARD_BITS_4K;/);
  assert.match(caseBody("default"), /guardBitsOption = DCP_GUARD_BITS_2K;/);
  assert.match(wrapper, /extra_options\[\] = \{guardBitsOption, NULL\}/);
});
