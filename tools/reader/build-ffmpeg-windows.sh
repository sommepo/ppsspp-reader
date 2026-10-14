#!/usr/bin/env bash
# Build the emulator's own FFmpeg (the ffmpeg/ submodule: the decoders a PSP game needs, and
# nothing else) for Windows with MinGW. The libraries the submodule ships for Windows are
# for another compiler and cannot be linked with this one.
#   tools/reader/build-ffmpeg-windows.sh
# Output: build-windows/ffmpeg/install (include/, lib/*.a). Built outside the submodule,
# which stays as it is. Once is enough.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/env.sh"
root="$(cd "$here/../.." && pwd)"
build="$root/build-windows/ffmpeg"
[ -f "$build/install/lib/libavcodec.a" ] && { echo "already built: $build/install"; exit 0; }
mkdir -p "$build/obj"
cd "$build/obj"
export LD_LIBRARY_PATH="$READER_MINGW/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$READER_MINGW/bin:$READER_HOME/tools/bin:$PATH"
"$here/limit.sh" -m 6G "$root/ffmpeg/configure" \
  --prefix="$build/install" \
  --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- \
  --cc=x86_64-w64-mingw32-gcc-posix --enable-cross-compile \
  --disable-shared --enable-static \
  --extra-cflags="-D__STDC_CONSTANT_MACROS -O3 -Wno-incompatible-pointer-types -Wno-int-conversion" \
  --disable-zlib --disable-yasm --disable-everything \
  --disable-avdevice --disable-filters --disable-programs --disable-network --disable-avfilter \
  --disable-postproc --disable-encoders --disable-doc \
  --disable-dxva2 --disable-d3d11va \
  --enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=h263 --enable-decoder=h263p \
  --enable-decoder=mpeg2video --enable-decoder=mjpeg --enable-decoder=mjpegb \
  --enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=atrac3 --enable-decoder=atrac3p \
  --enable-decoder=mp3 --enable-decoder=pcm_s16le --enable-decoder=pcm_s8 \
  --enable-encoder=ffv1 --enable-encoder=huffyuv --enable-encoder=mpeg4 --enable-encoder=pcm_s16le \
  --enable-demuxer=h264 --enable-demuxer=h263 --enable-demuxer=m4v --enable-demuxer=mpegps \
  --enable-demuxer=mpegvideo --enable-demuxer=avi --enable-demuxer=mp3 --enable-demuxer=aac \
  --enable-demuxer=pmp --enable-demuxer=oma --enable-demuxer=pcm_s16le --enable-demuxer=pcm_s8 \
  --enable-demuxer=wav --enable-muxer=avi \
  --enable-parser=h264 --enable-parser=mpeg4video --enable-parser=mpegvideo --enable-parser=aac \
  --enable-parser=aac_latm --enable-parser=mpegaudio --enable-protocol=file
"$here/limit.sh" -m 8G make -j"$READER_JOBS"
"$here/limit.sh" -m 4G make install
