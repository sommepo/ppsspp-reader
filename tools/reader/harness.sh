#!/usr/bin/env bash
# Start the headless harness with a game, under a memory limit. It draws with the emulator's
# software renderer: no GPU and no display are needed.
#   tools/reader/harness.sh [--rom FILE] [harness arguments...] &
#   tools/reader/hc.py "run 600" "shot private/harness/shots/a.png"
# Home is private/harness (the emulated memory stick is private/harness/memstick): it never
# touches a real PPSSPP setup. Talks on private/harness/sock.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
home="${READER_HARNESS_HOME:-$root/private/harness}"
rom="${READER_GAME:-$root/private/game.iso}"
rest=()
while [ $# -gt 0 ]; do
  case "$1" in
    --rom) rom="$2"; shift 2 ;;
    *) rest+=("$1"); shift ;;
  esac
done
built="$root/build-host/reader/ppsspp-reader-harness"
[ -x "$built" ] || { echo "harness.sh: the harness is not built yet: tools/reader/build-host.sh" >&2; exit 1; }
[ -e "$rom" ] || { echo "harness.sh: no game at $rom: name yours with --rom FILE" >&2; exit 1; }
mkdir -p "$home"
socket=(--socket "$home/sock")
for arg in "${rest[@]}"; do
  case "$arg" in --socket) socket=() ;; esac
done
export LD_LIBRARY_PATH="$READER_MESA_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Run a copy: the build replaces the binary in place, which takes a running harness down.
binary="$home/ppsspp-reader-harness.$$"
cp "$built" "$binary"
trap 'rm -f "$binary"' EXIT
"$here/limit.sh" -m "${READER_HARNESS_MEMORY_MAX:-4G}" \
  "$binary" --home "$home" --rom "$rom" --assets "$root/assets" "${socket[@]}" "${rest[@]}"
