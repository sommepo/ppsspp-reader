#!/usr/bin/env bash
# Build the Android app (arm64).
#   tools/reader/build-android.sh [gradle task] [gradle arguments...]
# Output: android/build/outputs/apk/reader/optimized/android-reader-optimized.apk
# Environment: JAVA_HOME (a JDK 17 or newer) and ANDROID_HOME, see env.sh.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
task="${1:-:android:assembleReaderOptimized}"; shift || true
ccache="$READER_HOST_PREFIX/bin/ccache"
cmake_args="-DCMAKE_JOB_POOLS=compile=${READER_JOBS};link=2 -DCMAKE_JOB_POOL_COMPILE=compile -DCMAKE_JOB_POOL_LINK=link -DCMAKE_C_COMPILER_LAUNCHER=$ccache -DCMAKE_CXX_COMPILER_LAUNCHER=$ccache -DUSE_CCACHE=OFF"
extra=()
[ -n "${READER_NDK:-}" ] && extra+=("-PREADER_NDK=$READER_NDK")
cd "$here/../.."
exec "$here/limit.sh" -m "${READER_BUILD_MEMORY_MAX:-12G}" ./gradlew --no-daemon --max-workers="${READER_GRADLE_WORKERS:-4}" \
  -PREADER_CMAKE_ARGS="$cmake_args" "${extra[@]}" "$task" "$@"
