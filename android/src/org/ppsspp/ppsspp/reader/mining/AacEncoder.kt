// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.File
import java.nio.ByteOrder

/** Encodes PCM to AAC-LC in an MPEG-4 container (.m4a), which Anki plays on every platform. */
object AacEncoder {
    private const val TIMEOUT_US = 10_000L

    /**
     * Writes [pcm] (interleaved, [channels] channels at [sampleRate] Hz) to [output].
     * [sampleRate] must be a standard AAC rate such as 44100 or 48000. Throws on failure.
     */
    fun encode(
        pcm: ShortArray,
        sampleRate: Int,
        channels: Int,
        output: File,
        bitRate: Int = 96_000
    ) {
        val format =
            MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, sampleRate, channels)
        format.setInteger(
            MediaFormat.KEY_AAC_PROFILE,
            MediaCodecInfo.CodecProfileLevel.AACObjectLC
        )
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16 * 1024)

        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        var muxer: MediaMuxer? = null
        var muxerStarted = false
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
            muxer = MediaMuxer(output.path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            val info = MediaCodec.BufferInfo()
            var track = -1
            var offset = 0 // samples consumed
            var inputDone = false
            var outputDone = false
            while (!outputDone) {
                var fed = false
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(0)
                    if (index >= 0) {
                        val buffer = codec.getInputBuffer(index)!!
                        buffer.clear()
                        var count = minOf(buffer.remaining() / 2, pcm.size - offset)
                        count -= count % channels
                        buffer.order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(pcm, offset, count)
                        val pts = (offset / channels) * 1_000_000L / sampleRate
                        offset += count
                        inputDone = offset >= pcm.size || count == 0
                        codec.queueInputBuffer(
                            index,
                            0,
                            count * 2,
                            pts,
                            if (inputDone) MediaCodec.BUFFER_FLAG_END_OF_STREAM else 0
                        )
                        fed = true
                    }
                }
                // Take what the encoder has ready. Waiting is only for when there was nothing
                // to give it: a wait after every piece of input made a 20 second clip take 8.
                var wait = if (fed) 0L else TIMEOUT_US
                while (true) {
                    val index = codec.dequeueOutputBuffer(info, wait)
                    wait = 0L
                    if (index == MediaCodec.INFO_TRY_AGAIN_LATER) break
                    if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        track = muxer.addTrack(codec.outputFormat)
                        muxer.start()
                        muxerStarted = true
                    } else if (index >= 0) {
                        val buffer = codec.getOutputBuffer(index)!!
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) info.size = 0
                        if (info.size > 0 && muxerStarted) {
                            buffer.position(info.offset)
                            buffer.limit(info.offset + info.size)
                            muxer.writeSampleData(track, buffer, info)
                        }
                        codec.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            outputDone = true
                            break
                        }
                    }
                }
            }
        } finally {
            try {
                codec.stop()
            } catch (_: Exception) {
            }
            codec.release()
            try {
                if (muxerStarted) muxer?.stop()
            } catch (_: Exception) {
            }
            muxer?.release()
        }
    }
}
