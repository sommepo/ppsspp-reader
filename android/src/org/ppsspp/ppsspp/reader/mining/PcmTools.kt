// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import kotlin.math.abs
import kotlin.math.floor

/** Helpers for interleaved 16-bit PCM. */
object PcmTools {
    /**
     * Resamples interleaved PCM with linear interpolation. Good enough for speech taken from a
     * 32.7 kHz console mix, and cheap.
     */
    fun resample(input: ShortArray, channels: Int, fromRate: Int, toRate: Int): ShortArray {
        require(channels > 0 && fromRate > 0 && toRate > 0)
        val inFrames = input.size / channels
        if (inFrames == 0 || fromRate == toRate) return input.copyOf(inFrames * channels)
        val outFrames = (inFrames.toLong() * toRate / fromRate).toInt()
        val out = ShortArray(outFrames * channels)
        val step = fromRate.toDouble() / toRate
        for (i in 0 until outFrames) {
            val position = i * step
            val index = floor(position).toInt().coerceAtMost(inFrames - 1)
            val next = (index + 1).coerceAtMost(inFrames - 1)
            val fraction = position - index
            for (c in 0 until channels) {
                val a = input[index * channels + c].toInt()
                val b = input[next * channels + c].toInt()
                out[i * channels + c] = (a + (b - a) * fraction).toInt().toShort()
            }
        }
        return out
    }

    /**
     * Drops near-silent audio from the end, keeping [paddingFrames] after the last frame louder
     * than [threshold]. Returns an empty array when everything is silent.
     */
    fun trimTrailingSilence(
        input: ShortArray,
        channels: Int,
        threshold: Int,
        paddingFrames: Int
    ): ShortArray {
        val frames = input.size / channels
        var last = -1
        var frame = frames - 1
        while (frame >= 0 && last < 0) {
            for (c in 0 until channels) {
                if (abs(input[frame * channels + c].toInt()) > threshold) {
                    last = frame
                    break
                }
            }
            frame--
        }
        if (last < 0) return ShortArray(0)
        val keep = (last + 1 + paddingFrames).coerceAtMost(frames)
        return input.copyOf(keep * channels)
    }

    /** Fades the first and last [frames] frames to avoid clicks at the clip edges. */
    fun fadeEdges(samples: ShortArray, channels: Int, frames: Int) {
        val total = samples.size / channels
        val n = frames.coerceAtMost(total / 2)
        for (i in 0 until n) {
            val gain = i.toFloat() / n
            for (c in 0 until channels) {
                val head = i * channels + c
                val tail = (total - 1 - i) * channels + c
                samples[head] = (samples[head] * gain).toInt().toShort()
                samples[tail] = (samples[tail] * gain).toInt().toShort()
            }
        }
    }
}
