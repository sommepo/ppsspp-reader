#!/usr/bin/env bash
# Configure and build for this machine: the headless harness and the C++ tests.
#   tools/reader/build-host.sh [ninja targets...]
# Output: build-host/
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
build="$root/build-host"
P="$READER_HOST_PREFIX/bin"
# Compilers run under a memory limit like everything else here (see limit.sh).
limit=("$here/limit.sh" -m "${READER_BUILD_MEMORY_MAX:-12G}")
if [ ! -f "$build/build.ninja" ]; then
  "${limit[@]}" "$READER_CMAKE" -S "$root" -B "$build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$P/ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$P/x86_64-conda-linux-gnu-gcc" \
    -DCMAKE_CXX_COMPILER="$P/x86_64-conda-linux-gnu-g++" \
    -DCMAKE_AR="$P/x86_64-conda-linux-gnu-gcc-ar" \
    -DCMAKE_RANLIB="$P/x86_64-conda-linux-gnu-gcc-ranlib" \
    -DCMAKE_C_COMPILER_LAUNCHER="$P/ccache" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$P/ccache" \
    -DUSE_CCACHE=OFF -DCMAKE_C_FLAGS=-DGLEW_NO_GLU -DCMAKE_CXX_FLAGS=-DGLEW_NO_GLU \
    -DCMAKE_PREFIX_PATH="$READER_MESA_PREFIX" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,-rpath,$READER_MESA_PREFIX/lib -Wl,-rpath-link,$READER_MESA_PREFIX/lib" \
    -DCMAKE_JOB_POOLS="compile=${READER_JOBS};link=2" \
    -DCMAKE_JOB_POOL_COMPILE=compile -DCMAKE_JOB_POOL_LINK=link \
    -DHEADLESS=OFF -DUNITTEST=OFF -DATLAS_TOOL=OFF \
    -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=OFF -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF \
    -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_LIBSDL2=ON \
    -DREADER_HARNESS=ON -DREADER_TESTS=ON \
    ${READER_HOST_CMAKE_ARGS:-}
fi
if [ "${1:-}" = "--configure-only" ]; then exit 0; fi
exec "${limit[@]}" "$P/ninja" -C "$build" "$@"
