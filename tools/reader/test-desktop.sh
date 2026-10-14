#!/usr/bin/env bash
# Run the tests of the desktop reading logic (dictionaries, lookup, cards, statistics).
#   tools/reader/test-desktop.sh [Catch2 arguments...]
# The desktop build has to be configured with them: READER_DESKTOP_CMAKE_ARGS=-DREADER_TESTS=ON
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
"$here/build-desktop.sh" reader_desktop_tests
export QT_QPA_PLATFORM=offscreen
export QT_PLUGIN_PATH="$READER_QT_LINUX/plugins"
export LD_LIBRARY_PATH="$READER_QT_LINUX/lib:$READER_QT_SYSLIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/limit.sh" -m "${READER_TEST_MEMORY_MAX:-4G}" \
  "$root/build-desktop/reader/desktop/tests/reader_desktop_tests" "$@"
