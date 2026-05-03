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

The script writes generated artifacts to `dist/`.

## What This Is Not

This is not a full DCP authoring library. It does not generate MXF, CPL, PKL,
ASSETMAP, VOLINDEX, KDM, DKDM, or encrypted AS-DCP packages.

## Third-Party Notices

OpenJPEG is developed by the OpenJPEG project and is licensed under the
2-clause BSD License. See `NOTICE`.

## License

The FilmProcessor wrapper and build script are licensed under BSD-2-Clause.
OpenJPEG itself remains under its upstream license.
