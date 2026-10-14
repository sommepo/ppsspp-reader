#!/usr/bin/env bash
# Run the JVM tests of the Android reading layer.
#   tools/reader/test-android.sh [--tests 'org.ppsspp.ppsspp.reader.X'] [gradle arguments...]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
extra=()
[ -n "${READER_NDK:-}" ] && extra+=("-PREADER_NDK=$READER_NDK")
cd "$here/../.."
exec "$here/limit.sh" -m "${READER_BUILD_MEMORY_MAX:-12G}" ./gradlew --no-daemon --max-workers="${READER_GRADLE_WORKERS:-4}" \
  "${extra[@]}" :android:testReaderDebugUnitTest "$@"
