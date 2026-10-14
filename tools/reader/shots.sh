#!/usr/bin/env bash
# Tile screenshots into one sheet, scaled down, to look at many at once.
#   tools/reader/shots.sh OUT.png IN.png... [--cols N] [--scale F]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
CONDA_PREFIX="$READER_RE_PREFIX" exec "$READER_RE_PYTHON" "$here/montage.py" "$@"
