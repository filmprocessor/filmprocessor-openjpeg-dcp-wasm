#!/bin/bash
#
# Build OpenJPEG Wasm for DCP Studio
# Compiles OpenJPEG 2.5.0 with the DCP wrapper to WebAssembly
#
# Prerequisites:
#   - Emscripten SDK (emsdk) installed and activated
#   - Internet access (to download OpenJPEG source if not cached)
#
# Output:
#   openjpeg-dcp.js   - Emscripten JS glue code (ES6 module)
#   openjpeg-dcp.wasm - WebAssembly binary
#
# Usage:
#   ./build_openjpeg_wasm.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
DIST_DIR="${REPO_DIR}/dist"
OPENJPEG_VERSION="2.5.0"
OPENJPEG_DIR="${BUILD_DIR}/openjpeg-${OPENJPEG_VERSION}"
OPENJPEG_TARBALL="${BUILD_DIR}/openjpeg-v${OPENJPEG_VERSION}.tar.gz"
OPENJPEG_URL="https://github.com/uclouvain/openjpeg/archive/refs/tags/v${OPENJPEG_VERSION}.tar.gz"
EMSDK_DIR="${EMSDK_DIR:-}"
WRAPPER_SRC="${REPO_DIR}/src/openjpeg_dcp_wrapper.c"

echo "=== OpenJPEG Wasm Build for DCP Studio ==="
echo "OpenJPEG version: ${OPENJPEG_VERSION}"
echo "Build directory:  ${BUILD_DIR}"
echo "emsdk directory:  ${EMSDK_DIR:-not set}"
echo "Features:         O3, LTO, 5MB stack, stack-overflow-check (no SIMD)"
echo ""

# Activate emsdk
if [ -n "${EMSDK_DIR}" ] && [ -f "${EMSDK_DIR}/emsdk_env.sh" ]; then
    echo "Activating emsdk..."
    source "${EMSDK_DIR}/emsdk_env.sh"
else
    echo "ERROR: EMSDK_DIR must point to an Emscripten SDK checkout containing emsdk_env.sh"
    exit 1
fi

# Verify emcc is available
if ! command -v emcc &> /dev/null; then
    echo "ERROR: emcc not found after activating emsdk"
    exit 1
fi
echo "emcc version: $(emcc --version | head -1)"
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"
mkdir -p "${DIST_DIR}"

# Download OpenJPEG source if not present
if [ ! -d "${OPENJPEG_DIR}" ]; then
    if [ ! -f "${OPENJPEG_TARBALL}" ]; then
        echo "Downloading OpenJPEG ${OPENJPEG_VERSION}..."
        curl -L -o "${OPENJPEG_TARBALL}" "${OPENJPEG_URL}"
    fi
    echo "Extracting OpenJPEG..."
    tar xzf "${OPENJPEG_TARBALL}" -C "${BUILD_DIR}"
fi

# Build OpenJPEG with Emscripten (static library) — NO SIMD
OPENJPEG_BUILD="${BUILD_DIR}/openjpeg-build-no-simd"
if [ ! -f "${OPENJPEG_BUILD}/bin/libopenjp2.a" ]; then
    echo "Building OpenJPEG as static library with Emscripten (no SIMD)..."
    rm -rf "${OPENJPEG_BUILD}"
    mkdir -p "${OPENJPEG_BUILD}"
    cd "${OPENJPEG_BUILD}"

    emcmake cmake "${OPENJPEG_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_C_FLAGS="-O3 -flto" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_CODEC=OFF \
        -DBUILD_MJ2=OFF \
        -DBUILD_JPWL=OFF \
        -DBUILD_JP3D=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_THIRDPARTY=OFF

    emmake make -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4) openjp2

    echo "OpenJPEG static library built successfully (no SIMD)"
else
    echo "OpenJPEG static library already built (cached)"
fi

cd "${REPO_DIR}"

# Find the static library
OPENJPEG_LIB=""
for candidate in \
    "${OPENJPEG_BUILD}/bin/libopenjp2.a" \
    "${OPENJPEG_BUILD}/lib/libopenjp2.a" \
    "${OPENJPEG_BUILD}/src/lib/openjp2/libopenjp2.a"; do
    if [ -f "${candidate}" ]; then
        OPENJPEG_LIB="${candidate}"
        break
    fi
done

if [ -z "${OPENJPEG_LIB}" ]; then
    echo "ERROR: Could not find libopenjp2.a"
    echo "Searching build directory..."
    find "${OPENJPEG_BUILD}" -name "libopenjp2.a" 2>/dev/null
    exit 1
fi

echo "Using OpenJPEG library: ${OPENJPEG_LIB}"

# Find OpenJPEG headers
OPENJPEG_INCLUDE="${OPENJPEG_DIR}/src/lib/openjp2"
OPENJPEG_BUILD_INCLUDE="${OPENJPEG_BUILD}/src/lib/openjp2"

echo ""
echo "Compiling DCP wrapper to WebAssembly (O3 + LTO, no SIMD)..."

emcc -O3 -flto \
    "${WRAPPER_SRC}" \
    "${OPENJPEG_LIB}" \
    -I"${OPENJPEG_INCLUDE}" \
    -I"${OPENJPEG_BUILD_INCLUDE}" \
    -s WASM=1 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s EXPORTED_FUNCTIONS='["_openjpeg_encode_xyz","_openjpeg_decode_j2k","_openjpeg_free_buffer","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","setValue","getValue","HEAP32","HEAPU8"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=268435456 \
    -s MAXIMUM_MEMORY=1073741824 \
    -s STACK_SIZE=5242880 \
    -s STACK_OVERFLOW_CHECK=2 \
    -s NO_EXIT_RUNTIME=1 \
    -o "${DIST_DIR}/openjpeg-dcp.js"

echo ""
echo "=== Build Complete ==="
echo "Output files:"
ls -lh "${DIST_DIR}/openjpeg-dcp.js" "${DIST_DIR}/openjpeg-dcp.wasm"
echo ""
echo "Done."
