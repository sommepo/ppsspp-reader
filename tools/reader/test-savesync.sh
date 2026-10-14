#!/usr/bin/env bash
# Tests of save sync (reader/save_sync/tests), with made-up saves. Those that need a service
# get one: the Eden Reader fork's standalone service (its tools/reader/savesync/server.py,
# READER_SAVESYNC_SERVER), started here twice on loopback ports the system picks, each with a
# folder of its own that is removed afterwards. A service someone uses is never touched.
# Without that file the tests that need a service are skipped.
#   tools/reader/test-savesync.sh [Catch2 arguments...]
# The desktop build has to be configured with them: READER_DESKTOP_CMAKE_ARGS=-DREADER_TESTS=ON
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
server="${READER_SAVESYNC_SERVER:-$root/../eden-reader-fork/tools/reader/savesync/server.py}"

"$here/build-desktop.sh" reader_save_sync_tests

data="$(mktemp -d "${TMPDIR:-/tmp}/savesync-test.XXXXXX")"
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$data"
}
trap cleanup EXIT

# start NAME: a service with the folder $data/NAME; prints its port.
start() {
  mkdir "$data/$1"
  python3 "$server" --data "$data/$1" serve --bind 127.0.0.1 --port 0 >"$data/$1.log" 2>&1 &
  pids+=($!)
  local port=""
  for _ in $(seq 100); do
    port="$(sed -n 's/^save sync on 127\.0\.0\.1:\([0-9]*\),.*/\1/p' "$data/$1.log")"
    [ -n "$port" ] && break
    sleep 0.1
  done
  if [ -z "$port" ]; then
    echo "the test service did not start:" >&2
    cat "$data/$1.log" >&2
    exit 1
  fi
  started_port="$port"
}

if [ -f "$server" ]; then
  start one
  export SAVESYNC_TEST_ADDRESS="127.0.0.1:$started_port"
  SAVESYNC_TEST_CODE="$(python3 "$server" --data "$data/one" code)"
  export SAVESYNC_TEST_CODE
  export SAVESYNC_TEST_SERVICE_DATA="$data/one"
  start two
  export SAVESYNC_TEST_OTHER_ADDRESS="127.0.0.1:$started_port"
  SAVESYNC_TEST_OTHER_CODE="$(python3 "$server" --data "$data/two" code)"
  export SAVESYNC_TEST_OTHER_CODE
else
  echo "no service at $server: the tests that need one are skipped" >&2
fi
export LD_LIBRARY_PATH="$READER_QT_LINUX/lib:$READER_QT_SYSLIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$here/limit.sh" -m "${READER_TEST_MEMORY_MAX:-2G}" \
  "$root/build-desktop/reader/save_sync/tests/reader_save_sync_tests" "$@"
