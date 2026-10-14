#!/usr/bin/env bash
# Runs a command with a hard memory limit, so that a runaway process ends itself instead of
# exhausting the machine (a build of the emulator and a run of a game are heavy).
#   tools/reader/limit.sh [-m SIZE] command [arguments...]
# SIZE is a systemd size such as 2G; the default is READER_MEMORY_MAX or 4G. The command runs
# in its own transient scope: if it goes over the limit, only that scope is killed.
set -euo pipefail
limit="${READER_MEMORY_MAX:-4G}"
if [ "${1:-}" = "-m" ]; then
  limit="$2"
  shift 2
fi
if [ $# -eq 0 ]; then
  echo "usage: limit.sh [-m SIZE] command [arguments...]" >&2
  exit 64
fi
if ! systemd-run --user --scope --quiet -p MemoryMax=1G -- true 2>/dev/null; then
  # No way to cap it here (no systemd user session: a container, some WSL setups). A machine
  # that must not run uncapped says so in private/env.sh (READER_REQUIRE_MEMORY_LIMIT=1).
  private="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/private/env.sh"
  # shellcheck disable=SC1090
  [ -f "$private" ] && . "$private"
  if [ "${READER_REQUIRE_MEMORY_LIMIT:-}" = 1 ] && [ "${READER_NO_MEMORY_LIMIT:-}" != 1 ]; then
    echo "limit.sh: no systemd user scope available, refusing to run without a memory limit" >&2
    echo "          (set READER_NO_MEMORY_LIMIT=1 to run anyway)" >&2
    exit 69
  fi
  [ -n "${READER_LIMIT_NOTED:-}" ] || echo "limit.sh: no systemd user session here: running without the $limit memory limit" >&2
  export READER_LIMIT_NOTED=1
  exec "$@"
fi
exec systemd-run --user --scope --quiet --collect \
  -p MemoryMax="$limit" -p MemorySwapMax=0 -- "$@"
