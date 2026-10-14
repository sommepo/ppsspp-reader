#!/usr/bin/env bash
# Run the desktop app (the Linux build) without a display, under a memory limit, to try the
# reader's windows: on a Weston compositor that draws to nothing, with Xwayland, and Mesa's
# software OpenGL. tools/reader/dc.py talks to it.
#   tools/reader/desktop.sh [--rom FILE] [arguments for the app...] &
#   tools/reader/dc.py status
# Its home is private/desktop/home: it never touches a real PPSSPP setup.
# Needs, in READER_HOME/weston: weston, xwayland and the xcb-util libraries (conda-forge).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
home="${READER_DESKTOP_HOME:-$root/private/desktop}"
weston="${READER_WESTON_PREFIX:-$READER_HOME/weston}"
rom=""
rest=()
while [ $# -gt 0 ]; do
  case "$1" in
    --rom) rom="$2"; shift 2 ;;
    *) rest+=("$1"); shift ;;
  esac
done
[ -x "$root/build-desktop/PPSSPPQt" ] || { echo "desktop.sh: not built yet: tools/reader/build-desktop.sh PPSSPPQt" >&2; exit 1; }
mkdir -p "$home/home" "$home/run" "$home/fontconfig"
chmod 700 "$home/run"
# The stand-in screen, once: it stays for the next run.
if ! [ -S "$home/run/reader-wl" ] || ! pgrep -u "$(id -u)" -f "weston.*--socket=reader-wl" >/dev/null; then
  rm -f "$home/run/reader-wl" "$home/run/reader-wl.lock"
  setsid nohup "$here/limit.sh" -m 1G env XDG_RUNTIME_DIR="$home/run" LD_LIBRARY_PATH="$weston/lib" \
    PATH="$weston/bin:$PATH" "$weston/bin/weston" --backend=headless --socket=reader-wl \
    --width="${READER_DESKTOP_WIDTH:-1280}" --height="${READER_DESKTOP_HEIGHT:-720}" \
    --renderer=pixman --idle-time=0 --xwayland > "$home/weston.log" 2>&1 &
  sleep 3
fi
display="$(grep -o 'listening on display :[0-9]*' "$home/weston.log" | tail -1 | grep -o ':[0-9]*')"
[ -n "$display" ] || { echo "desktop.sh: the stand-in screen did not start: $home/weston.log" >&2; exit 1; }
cat > "$home/fontconfig/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$(dirname "$READER_PREVIEW_FONT_FILE")</dir>
  <cachedir>$home/fontconfig/cache</cachedir>
</fontconfig>
CONF
args=()
[ -n "$rom" ] && args+=("$(realpath "$rom")")
# Run a copy: the build replaces the binary in place, which takes a running app down.
binary="$home/PPSSPPQt.$$"
cp "$root/build-desktop/PPSSPPQt" "$binary"
trap 'rm -f "$binary"' EXIT
cd "$root/build-desktop"
"$here/limit.sh" -m "${READER_DESKTOP_MEMORY_MAX:-4G}" env HOME="$home/home" DISPLAY="$display" \
  QT_QPA_PLATFORM=xcb QT_XCB_GL_INTEGRATION=xcb_egl QT_PLUGIN_PATH="$READER_QT_LINUX/plugins" \
  LD_LIBRARY_PATH="$READER_MESA_PREFIX/lib:$READER_QT_LINUX/lib:$READER_QT_SYSLIBS:$weston/lib" \
  __EGL_VENDOR_LIBRARY_DIRS="$READER_MESA_PREFIX/share/glvnd/egl_vendor.d" LIBGL_ALWAYS_SOFTWARE=1 \
  FONTCONFIG_FILE="$home/fontconfig/fonts.conf" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}" \
  READER_DESKTOP_CONTROL="$home/sock" "$binary" "${args[@]}" "${rest[@]}"
