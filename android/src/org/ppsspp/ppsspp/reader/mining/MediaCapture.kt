// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.ReaderLog as Log

/**
 * Screenshots and audio clips of lines, for flashcards.
 *
 * A screenshot of each line is kept for the most recent lines so that a card made from the
 * backlog shows the scene of that line rather than the current one. Audio comes from the core's
 * history of emulated output: everything the game played, not an isolated voice track.
 *
 * Main thread unless noted; encoding and file work run on a background thread.
 */
class MediaCapture(context: Context, private val settings: ReaderSettings) : CardMedia {
    /**
     * Copies the game picture into a bitmap no wider than the given width and passes it (or null)
     * to the callback on the main thread. Provided by the emulation screen while it is visible.
     */
    var frameSource: ((maxWidth: Int, callback: (Bitmap?) -> Unit) -> Unit)? = null

    private val main = Handler(Looper.getMainLooper())
    private val background = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderMedia") }
    private val directory = File(context.cacheDir, "reader/media").apply { mkdirs() }
    private val pending = HashMap<Long, Runnable>()
    private val captured = LinkedHashSet<Long>()

    /** The audio of a line, copied out of the history before the history moved on. */
    private class Held(val backlogId: Long, val from: Long, val samples: ShortArray)

    @Volatile
    private var held: Held? = null
    private var holding = -1L

    init {
        // Screenshots of an earlier run belong to lines that can no longer be matched reliably.
        background.execute { directory.listFiles()?.forEach { it.delete() } }
    }

    /**
     * Keeps a screenshot for the backlog line [backlogId]: at once when [immediately], otherwise
     * after the line had time to appear. Does nothing when the line already has one.
     */
    fun captureForLine(backlogId: Long, immediately: Boolean) {
        if (backlogId < 0 || settings.screenshotCacheLines == 0) return
        if (backlogId in captured) return
        pending.remove(backlogId)?.let { main.removeCallbacks(it) }
        if (immediately) {
            captureInto(backlogId)
        } else {
            val task = Runnable {
                pending.remove(backlogId)
                captureInto(backlogId)
            }
            pending[backlogId] = task
            main.postDelayed(task, LINE_SETTLE_MS)
        }
    }

    /** The kept screenshot of a backlog line, or null. */
    override fun screenshotFor(backlogId: Long): File? =
        fileFor(backlogId).takeIf { backlogId in captured && it.exists() }

    /** Takes a screenshot of the current frame into a new file. */
    override fun captureNow(callback: (File?) -> Unit) {
        val source = frameSource
        if (source == null) {
            callback(null)
            return
        }
        source(settings.screenshotMaxWidth) { bitmap ->
            if (bitmap == null) {
                callback(null)
            } else {
                val file = File(directory, "now-${System.currentTimeMillis()}.jpg")
                background.execute {
                    val ok = writeJpeg(bitmap, file)
                    main.post { callback(if (ok) file else null) }
                }
            }
        }
    }

    /**
     * Call about once a second with the newest line, whose audio began at history position
     * [start]. The history only goes back so far, and a line that is read slowly would lose its
     * audio: once a clip's worth of it has played, it is copied out and kept until another line
     * has stayed as long.
     */
    fun keepAudioOf(backlogId: Long, start: Long) {
        if (!settings.captureAudio || start < 0 || backlogId < 0 || holding == backlogId) return
        val rate = ReaderNative.audioSampleRate()
        val from = clipStart(start, rate)
        val to = from + settings.audioClipMaxSeconds.toLong() * rate
        if (ReaderNative.audioPosition() < to) return
        holding = backlogId
        background.execute {
            try {
                val actual = LongArray(1)
                val samples = ReaderNative.readAudio(from, to, actual)
                if (samples != null && actual[0] == from) held = Held(backlogId, from, samples)
            } catch (e: Exception) {
                Log.error("[Reader] Could not keep a line's audio: ${e.message}")
            }
        }
    }

    private fun clipStart(start: Long, rate: Int): Long =
        (start - settings.audioLeadMs.toLong() * rate / 1000).coerceAtLeast(0)

    /**
     * Encodes the emulated audio of line [backlogId] between two history positions as an .m4a
     * clip. [end] may be null for "until now". The clip is limited to the configured maximum
     * length and trailing silence is removed. The callback receives null when there is nothing
     * audible, or when the audio is no longer there.
     */
    override fun audioClip(backlogId: Long, start: Long, end: Long?, callback: (File?) -> Unit) {
        if (!settings.captureAudio || start < 0) {
            callback(null)
            return
        }
        val rate = ReaderNative.audioSampleRate()
        val from = clipStart(start, rate)
        val limit = from + settings.audioClipMaxSeconds.toLong() * rate
        val to = minOf(end ?: ReaderNative.audioPosition(), limit)
        background.execute {
            val file = try {
                val actual = LongArray(1)
                var samples = ReaderNative.readAudio(from, to, actual)
                if (samples == null || actual[0] > from) {
                    // The history has moved past the start of the line: use what was kept.
                    val kept = held?.takeIf { it.backlogId == backlogId && it.from == from }
                    if (kept != null) {
                        val wanted = ((to - from) * CHANNELS).coerceIn(0L, kept.samples.size.toLong()).toInt()
                        samples = kept.samples.copyOf(wanted)
                    }
                }
                if (samples == null || samples.size < rate / 5 * CHANNELS) {
                    null
                } else {
                    val trimmed = PcmTools.trimTrailingSilence(
                        samples,
                        CHANNELS,
                        SILENCE_THRESHOLD,
                        rate / 4
                    )
                    if (trimmed.size < rate / 5 * CHANNELS) {
                        null
                    } else {
                        val resampled = PcmTools.resample(trimmed, CHANNELS, rate, OUTPUT_RATE)
                        PcmTools.fadeEdges(resampled, CHANNELS, OUTPUT_RATE / 100)
                        val began = System.currentTimeMillis()
                        File(directory, "clip-$began.m4a").also {
                            AacEncoder.encode(resampled, OUTPUT_RATE, CHANNELS, it)
                            Log.info(
                                "[Reader] Audio clip: %.1f s, loudest sample %d of 32767, encoded in %d ms".format(
                                    trimmed.size / CHANNELS / rate.toDouble(),
                                    trimmed.maxOf { s -> if (s < 0) -s.toInt() else s.toInt() },
                                    System.currentTimeMillis() - began
                                )
                            )
                        }
                    }
                }
            } catch (e: Exception) {
                Log.error("[Reader] Audio clip failed: ${e.message}")
                null
            }
            main.post { callback(file) }
        }
    }

    fun release() {
        for (task in pending.values) main.removeCallbacks(task)
        pending.clear()
        frameSource = null
        background.shutdown()
    }

    private fun captureInto(backlogId: Long) {
        val source = frameSource ?: return
        source(settings.screenshotMaxWidth) { bitmap ->
            if (bitmap != null && backlogId !in captured) {
                captured.add(backlogId)
                val evicted = ArrayList<Long>()
                while (captured.size > settings.screenshotCacheLines) {
                    val oldest = captured.first()
                    captured.remove(oldest)
                    evicted.add(oldest)
                }
                background.execute {
                    writeJpeg(bitmap, fileFor(backlogId))
                    for (id in evicted) fileFor(id).delete()
                }
            }
        }
    }

    private fun fileFor(backlogId: Long) = File(directory, "line-$backlogId.jpg")

    private fun writeJpeg(bitmap: Bitmap, file: File): Boolean = try {
        val cropped = Letterbox.trim(bitmap)
        file.outputStream().use { cropped.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, it) }
        if (cropped !== bitmap) cropped.recycle()
        bitmap.recycle()
        true
    } catch (e: Exception) {
        Log.error("[Reader] Screenshot failed: ${e.message}")
        false
    }

    companion object {
        private const val LINE_SETTLE_MS = 1500L
        private const val JPEG_QUALITY = 88
        private const val CHANNELS = 2
        private const val OUTPUT_RATE = 48_000
        private const val SILENCE_THRESHOLD = 180
    }
}
