#!/usr/bin/env bash
# Configure and build the desktop app for Windows (x86-64), cross-compiled with MinGW. It
# cannot be run on this machine (no Wine here): it is tried on a Windows computer.
#   tools/reader/build-windows.sh [ninja targets...]
# Output: build-windows/PPSSPPQt.exe; tools/reader/package-windows.sh makes a folder of it
# that runs on Windows.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
build="${READER_WINDOWS_BUILD:-$root/build-windows}"
P="$READER_HOST_PREFIX/bin"
limit=("$here/limit.sh" -m "${READER_BUILD_MEMORY_MAX:-12G}")
"$here/build-ffmpeg-windows.sh"
ffmpeg="$build/ffmpeg/install"
# The compiler was unpacked from its packages, not installed: it finds the libraries it was
# linked against here. Qt's own build tools (moc, rcc) are those of the Qt for this machine.
export LD_LIBRARY_PATH="$READER_MINGW/lib/x86_64-linux-gnu:$READER_QT_LINUX/lib:$READER_QT_SYSLIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The sources spell Windows' headers as Windows users do; MinGW on Linux has one spelling.
python3 "$here/win_headers.py" "$READER_MINGW/share/mingw-w64/include" "$build/win-include" \
  "$root/Common" "$root/Core" "$root/GPU" "$root/UI" "$root/Qt" "$root/Windows" "$root/ext" "$root/SDL" >/dev/null
if [ ! -f "$build/build.ninja" ]; then
  "${limit[@]}" "$READER_CMAKE" -S "$root" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$here/cmake/mingw-toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="$P/ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER="$P/ccache" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$P/ccache" \
    -DUSE_CCACHE=OFF \
    -DCMAKE_C_FLAGS="-isystem $build/win-include" -DCMAKE_CXX_FLAGS="-isystem $build/win-include" \
    -DCMAKE_JOB_POOLS="compile=${READER_JOBS};link=2" \
    -DCMAKE_JOB_POOL_COMPILE=compile -DCMAKE_JOB_POOL_LINK=link \
    -DQt6_DIR="$READER_QT_WINDOWS/lib/cmake/Qt6" \
    -DCMAKE_PREFIX_PATH="$READER_QT_WINDOWS;$READER_SDL2_WINDOWS" \
    -DQT_HOST_PATH="$READER_QT_LINUX" -DQT_HOST_PATH_CMAKE_DIR="$READER_QT_LINUX/lib/cmake" \
    -DSDL2_INCLUDE_DIR="$READER_SDL2_WINDOWS/include/SDL2" \
    -DSDL2_LIBRARY="$READER_SDL2_WINDOWS/lib/libSDL2.dll.a" \
    -DSDL2_RUNTIME_LIBRARY="$READER_SDL2_WINDOWS/bin/SDL2.dll" \
    -DSDL2_SDLMAIN_LIBRARY="$READER_SDL2_WINDOWS/lib/libSDL2main.a" \
    -DFFMPEG_DIR="$ffmpeg" -DFFmpeg_INCLUDE_DIR="$ffmpeg/include" \
    -DCMAKE_FIND_ROOT_PATH="$READER_MINGW/x86_64-w64-mingw32;$READER_QT_WINDOWS;$READER_SDL2_WINDOWS;$ffmpeg" \
    -DUSING_QT_UI=ON -DHEADLESS=OFF -DUNITTEST=OFF -DATLAS_TOOL=OFF \
    -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_LIBSDL2=ON \
    -DUSE_SYSTEM_ZSTD=OFF -DUSE_SYSTEM_SNAPPY=OFF \
    ${READER_WINDOWS_CMAKE_ARGS:-}
fi
if [ "${1:-}" = "--configure-only" ]; then exit 0; fi
exec "${limit[@]}" "$P/ninja" -C "$build" "$@"
