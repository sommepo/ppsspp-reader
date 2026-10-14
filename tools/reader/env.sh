# Source this file: where the toolchains are, for every script in tools/reader.
#
# Everything the builds need is in one folder outside the repository, READER_HOME
# (~/.local/share/ppsspp-reader unless set), which tools/reader/setup.sh fills without root.
# Each path below is a variable: set it before running a script to use something else.
# A file private/env.sh in the repository (not in Git) is read first, for a machine's own
# settings.
_reader_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
[ -f "$_reader_root/private/env.sh" ] && . "$_reader_root/private/env.sh"

export READER_HOME="${READER_HOME:-$HOME/.local/share/ppsspp-reader}"
# The compilers, the Android SDK and the analysis tools.
export READER_TOOLCHAINS="${READER_TOOLCHAINS:-$READER_HOME}"
# What the harness and the desktop builds need besides (Mesa, Qt, MinGW).
export READER_DESKTOP_TOOLCHAINS="${READER_DESKTOP_TOOLCHAINS:-$READER_HOME}"

# A GCC with its own C library, cmake, ninja, ccache, pkg-config and Python (conda-forge).
export READER_HOST_PREFIX="$READER_TOOLCHAINS/host"
if [ -z "${READER_CMAKE:-}" ]; then
  if [ -x "$READER_HOST_PREFIX/bin/cmake" ]; then
    READER_CMAKE="$READER_HOST_PREFIX/bin/cmake"
  else
    READER_CMAKE="cmake"
  fi
fi
export READER_CMAKE

# Android: a JDK and the SDK. Gradle fetches the NDK and its CMake into the SDK itself.
export JAVA_HOME="${JAVA_HOME:-$READER_TOOLCHAINS/jdk/lib/jvm}"
export ANDROID_HOME="${ANDROID_HOME:-$READER_TOOLCHAINS/android-sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"

# Mesa (software OpenGL, with EGL and the X11 client libraries it is built against): what
# the builds for this machine link, and what draws the desktop app without a display.
export READER_MESA_PREFIX="${READER_MESA_PREFIX:-$READER_HOME/gl}"
export CCACHE_DIR="${CCACHE_DIR:-$READER_HOME/ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-8G}"
export NDK_CCACHE="$READER_HOST_PREFIX/bin/ccache"
# Reverse-engineering helpers for writing text profiles: a Python with the Capstone
# disassembler and Pillow.
export READER_RE_PREFIX="$READER_TOOLCHAINS/re"
export READER_RE_PYTHON="$READER_RE_PREFIX/bin/python"
# A font with Japanese in it, for previews and for the desktop app without a display.
export READER_PREVIEW_FONT_FILE="${READER_PREVIEW_FONT_FILE:-$READER_TOOLCHAINS/fonts/NotoSansJP-Regular.otf}"
# Parallel compile jobs.
export READER_JOBS="${READER_JOBS:-8}"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

# The desktop app (Qt): one build for Linux, which is what the desktop reading layer is
# developed and tried with, and one for Windows, cross-compiled with MinGW.
export READER_QT_VERSION="${READER_QT_VERSION:-6.9.3}"
export READER_QT_LINUX="${READER_QT_LINUX:-$READER_DESKTOP_TOOLCHAINS/qt/linux-amd64-$READER_QT_VERSION}"
export READER_QT_SYSLIBS="${READER_QT_SYSLIBS:-$READER_DESKTOP_TOOLCHAINS/qt/syslibs/root/usr/lib/x86_64-linux-gnu}"
export READER_QT_WINDOWS="${READER_QT_WINDOWS:-$READER_DESKTOP_TOOLCHAINS/qt/official/$READER_QT_VERSION/mingw_64}"
export READER_MINGW="${READER_MINGW:-$READER_DESKTOP_TOOLCHAINS/mingw/root/usr}"
# A checkout of VN Library (https://github.com/sommepo/vn-library), for its Anki add-on, which
# the Windows package carries. Only read. Without it the package is made without the add-on.
export READER_VN_LIBRARY="${READER_VN_LIBRARY:-$READER_HOME/vn-library}"

# For the Windows build: SDL's MinGW development package (controllers, sound), unpacked.
export READER_SDL2_WINDOWS="${READER_SDL2_WINDOWS:-$READER_HOME/sdl2-mingw/SDL2-2.32.10/x86_64-w64-mingw32}"
unset _reader_root
