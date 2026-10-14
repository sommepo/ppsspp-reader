// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.view.PixelCopy
import android.view.SurfaceView
import android.view.View
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderCatalog
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ReaderSettings

/**
 * What the reader overlays need from the emulation screen: opening the reader's own screens on
 * the game's display, and knowing whether the second display is the reader's.
 */
class ReaderHost(
    private val activity: Activity,
    private val secondaryActive: () -> Boolean,
    /** Called before one of the reader's screens is opened over the game. */
    private val onLeaving: () -> Unit = {}
) : ReaderOverlay.Host {
    override fun openBacklog() = open(ReaderBacklogActivity::class.java)

    override fun openStatistics() = open(ReaderStatsActivity::class.java)

    override fun openSettings() = open(ReaderSettingsActivity::class.java)

    override fun openDictionaries() = open(DictionaryManagerActivity::class.java)

    override fun openAnkiSetup() = open(AnkiSetupActivity::class.java)

    /**
     * Asks what the running game's cards do with their line's audio: what Reader settings say
     * for every game, or this game's own answer.
     */
    override fun chooseCardAudio() {
        val settings = ReaderSettings(activity)
        val game = ReaderSession.gameKey
        if (game.isEmpty()) return
        val names = activity.resources.getStringArray(R.array.reader_card_audio_entries)
        val values = activity.resources.getStringArray(R.array.reader_card_audio_values)
        fun name(choice: ReaderSettings.CardAudio) = names[values.indexOf(choice.value)]
        val choices = listOf<ReaderSettings.CardAudio?>(null) + ReaderSettings.CardAudio.values()
        val labels = choices.map { choice ->
            if (choice == null) {
                val said = ReaderCatalog.cardAudio(activity, game)
                if (said != null) {
                    activity.getString(R.string.reader_card_audio_as_game, name(said))
                } else {
                    activity.getString(R.string.reader_card_audio_as_general, name(settings.cardAudioGeneral))
                }
            } else {
                name(choice)
            }
        }
        MaterialAlertDialogBuilder(themed)
            .setTitle(R.string.reader_card_audio_game)
            .setSingleChoiceItems(labels.toTypedArray(), choices.indexOf(settings.cardAudioOf(game))) { dialog, which ->
                settings.setCardAudioOf(game, choices[which])
                dialog.dismiss()
            }
            .show()
    }

    /**
     * The game's own screen is the emulator's and has none of the colours the reader's
     * dialogs are made with: a dialog built on it ended the app. They are built on the
     * reader's theme over it.
     */
    private val themed: android.content.Context
        get() = android.view.ContextThemeWrapper(activity, R.style.Theme_Reader)

    override fun hasSecondaryScreen(): Boolean = secondaryActive()

    private fun open(screen: Class<*>) {
        onLeaving()
        activity.startActivity(Intent(activity, screen))
    }

    /**
     * The reader entry of the in-game menu: what the panel's buttons offer, for when the panel
     * is not there. A dialog rather than a menu at [anchor]: next to the drawer item there is
     * no room for the list on a small landscape screen.
     */
    fun showMenu(@Suppress("UNUSED_PARAMETER") anchor: View) {
        val settings = ReaderSettings(activity)
        fun toggle(label: Int, on: Boolean): String =
            (if (on) "\u2713  " else "     ") + activity.getString(label)
        val entries = listOf<Pair<String, () -> Unit>>(
            activity.getString(R.string.reader_mine_sentence) to {
                ReaderSession.dispatch(ReaderSession.Command.SENTENCE_CARD)
            },
            activity.getString(R.string.reader_statistics) to { openStatistics() },
            activity.getString(R.string.reader_backlog) to { openBacklog() },
            activity.getString(R.string.reader_dictionaries) to { openDictionaries() },
            activity.getString(R.string.reader_anki_setup) to { openAnkiSetup() },
            activity.getString(R.string.reader_card_audio_game) to { chooseCardAudio() },
            activity.getString(R.string.reader_settings) to { openSettings() },
            toggle(R.string.reader_pref_screen_text, settings.screenText) to {
                settings.preferences.edit()
                    .putBoolean(ReaderSettings.KEY_SCREEN_TEXT, !settings.screenText).apply()
            },
            toggle(R.string.reader_pref_panel_enabled, settings.panelEnabled) to {
                settings.preferences.edit()
                    .putBoolean(ReaderSettings.KEY_PANEL_ENABLED, !settings.panelEnabled).apply()
            },
            activity.getString(R.string.reader_reload_profile) to { ReaderNative.reloadProfile() }
        )
        MaterialAlertDialogBuilder(themed)
            .setTitle(R.string.reader_menu_title)
            .setItems(entries.map { it.first }.toTypedArray()) { _, which -> entries[which].second() }
            .show()
    }

    companion object {
        /**
         * Copies what [surface] shows into a bitmap at most [maxWidth] wide. Overlays and
         * on-screen controls are separate views and are not part of the copy.
         */
        fun copyFrame(surface: SurfaceView, maxWidth: Int, callback: (Bitmap?) -> Unit) {
            val width = surface.width
            val height = surface.height
            if (width <= 0 || height <= 0 || !surface.holder.surface.isValid) {
                callback(null)
                return
            }
            val scale = minOf(1f, maxWidth.toFloat() / width)
            val bitmap = Bitmap.createBitmap(
                (width * scale).toInt().coerceAtLeast(1),
                (height * scale).toInt().coerceAtLeast(1),
                Bitmap.Config.ARGB_8888
            )
            try {
                PixelCopy.request(
                    surface,
                    bitmap,
                    { result -> callback(if (result == PixelCopy.SUCCESS) bitmap else null) },
                    Handler(Looper.getMainLooper())
                )
            } catch (_: IllegalArgumentException) {
                callback(null) // the surface went away between the check and the request
            }
        }

        /** Hands the session a way to take screenshots of [surface] while it is on screen. */
        fun provideFrames(surface: SurfaceView) {
            ReaderSession.media?.frameSource = { maxWidth, callback ->
                copyFrame(surface, maxWidth, callback)
            }
        }
    }
}
