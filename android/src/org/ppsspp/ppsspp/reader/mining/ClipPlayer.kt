// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import android.media.AudioAttributes
import android.media.MediaPlayer
import java.io.File
import org.ppsspp.ppsspp.reader.ReaderLog as Log

/**
 * Plays a card's audio clip, for the user to hear what the card would get before it is made.
 * The game is not silenced meanwhile: what it plays now is heard along with the clip. Main thread.
 */
class ClipPlayer {
    private var player: MediaPlayer? = null

    val isPlaying: Boolean get() = player != null

    /**
     * Plays [file] from its start, in place of whatever was playing. [onEnd] runs when it has
     * played to its end or could not be played, not when it was stopped.
     */
    fun play(file: File, onEnd: () -> Unit) {
        stop()
        val created = MediaPlayer()
        player = created
        fun ended() {
            if (player !== created) return
            stop()
            onEnd()
        }
        try {
            created.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            created.setDataSource(file.path)
            created.setOnPreparedListener { if (player === created) created.start() }
            created.setOnCompletionListener { ended() }
            created.setOnErrorListener { _, what, extra ->
                Log.error("[Reader] The audio clip could not be played ($what, $extra)")
                ended()
                true
            }
            created.prepareAsync()
        } catch (e: Exception) {
            Log.error("[Reader] The audio clip could not be played: ${e.message}")
            ended()
        }
    }

    fun stop() {
        val playing = player ?: return
        player = null
        try {
            playing.release()
        } catch (_: Exception) {
            // Already gone.
        }
    }
}
