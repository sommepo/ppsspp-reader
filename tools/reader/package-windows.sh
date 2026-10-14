#!/usr/bin/env bash
# Make a folder of the Windows build that runs on Windows: the program, the libraries it
# needs (Qt, the compiler's runtime, SDL), Qt's plugins and the emulator's assets; and a zip
# of it.
#   tools/reader/package-windows.sh
# Output: build-windows/package/PPSSPPReader/ and build-windows/package/PPSSPPReader-windows.zip
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
build="${READER_WINDOWS_BUILD:-$root/build-windows}"
exe="$build/PPSSPPQt.exe"
out="$build/package/PPSSPPReader"
objdump="$READER_MINGW/bin/x86_64-w64-mingw32-objdump"
strip="$READER_MINGW/bin/x86_64-w64-mingw32-strip"
export LD_LIBRARY_PATH="$READER_MINGW/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -f "$exe" ] || { echo "no $exe: run tools/reader/build-windows.sh PPSSPPQt first" >&2; exit 1; }

# The way in: Qt's entry code calls qMain, which must hand on to the real main (SDL_main,
# renamed by SDL's header). The first package's qMain called itself and the program hung at
# its start without a window.
entry="$("$objdump" -d --disassemble=_Z5qMainiPPc "$exe")"
if ! grep -q 'SDL_main' <<<"$entry"; then
  echo "package-windows.sh: qMain does not reach SDL_main (Qt/windows_support.cpp):" >&2
  grep -A6 '<_Z5qMainiPPc>:' <<<"$entry" >&2 || echo "  (no qMain in $exe)" >&2
  exit 1
fi

rm -rf "$out"
mkdir -p "$out"
install -m 755 "$exe" "$out/ppsspp-reader.exe"
"$strip" --strip-unneeded "$out/ppsspp-reader.exe"

# The compiler's runtime libraries must be the ones of the compiler the program was built
# with: Qt ships older copies of the same names, and a program linked against the newer ones
# does not start with them.
gcc_dir="$(find "$READER_MINGW/lib/gcc/x86_64-w64-mingw32" -maxdepth 1 -name '*-posix' | sort -V | tail -1)"
[ -n "$gcc_dir" ] || { echo "no MinGW compiler under $READER_MINGW" >&2; exit 1; }
runtime=("$gcc_dir" "$READER_MINGW/x86_64-w64-mingw32/lib")
search=("${runtime[@]}" "$READER_QT_WINDOWS/bin" "$READER_SDL2_WINDOWS/bin")
find_dll() {
  local name="$1" dir found
  for dir in "${search[@]}"; do
    # Windows does not tell upper case from lower in file names.
    found="$(find "$dir" -maxdepth 1 -iname "$name" -print -quit 2>/dev/null)"
    if [ -n "$found" ]; then echo "$found"; return 0; fi
  done
  return 1
}
copy_with_deps() {
  local file="$1" dll path dir
  while read -r dll; do
    [ -n "$dll" ] || continue
    [ -e "$out/$dll" ] && continue
    if path="$(find_dll "$dll")"; then
      install -m 644 "$path" "$out/$(basename "$path")"
      for dir in "${runtime[@]}"; do
        [ "$(dirname "$path")" = "$dir" ] && "$strip" --strip-unneeded "$out/$(basename "$path")"
      done
      copy_with_deps "$path"
    fi
  done < <("$objdump" -p "$file" | awk '/DLL Name:/ {print $3}')
}
copy_with_deps "$exe"

for plugin in platforms/qwindows.dll styles/qmodernwindowsstyle.dll \
              imageformats/qjpeg.dll imageformats/qgif.dll imageformats/qico.dll \
              tls/qschannelbackend.dll; do
  mkdir -p "$out/$(dirname "$plugin")"
  install -m 644 "$READER_QT_WINDOWS/plugins/$plugin" "$out/$plugin"
  copy_with_deps "$READER_QT_WINDOWS/plugins/$plugin"
done

# The emulator's own files: fonts, its interface's pictures and texts, the PSP's fonts, and
# with them the bundled text profiles (which the program also carries inside itself).
cp -r "$root/assets" "$out/assets"
rm -rf "$out/assets/debugger"
# A published package (READER_PACKAGE_SCRIPTS=0) comes with no game scripts: they are installed
# from the published list.
if [ "${READER_PACKAGE_SCRIPTS:-1}" = "0" ]; then
  rm -f "$out"/assets/reader/profiles/*.lua "$out"/assets/reader/profiles/*.charset
fi

# Every function a file of the package takes from another file of the package must be there.
"$READER_HOST_PREFIX/bin/python3" "$here/pe_check.py" "$out"

(cd "$out/.." && rm -f PPSSPPReader-windows.zip && "$READER_HOST_PREFIX/bin/python3" -m zipfile -c PPSSPPReader-windows.zip PPSSPPReader)
# Beside it, the program as it was built, with its names: what makes a crash dump from
# Windows readable (the packaged one is stripped). Same code, to be put in the program's place.
(cd "$out/.." && rm -rf PPSSPPReader-windows-debug.zip debug && mkdir debug \
  && install -m 755 "$exe" debug/ppsspp-reader.exe \
  && "$READER_HOST_PREFIX/bin/python3" -m zipfile -c PPSSPPReader-windows-debug.zip debug/ppsspp-reader.exe \
  && rm -rf debug)
du -sh "$out" "$out/../PPSSPPReader-windows.zip" "$out/../PPSSPPReader-windows-debug.zip"
