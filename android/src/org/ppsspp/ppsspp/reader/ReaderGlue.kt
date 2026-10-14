// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.app.Activity
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import org.ppsspp.ppsspp.reader.cursor.HoverCursor
import org.ppsspp.ppsspp.reader.ui.ReaderHost
import org.ppsspp.ppsspp.reader.ui.ReaderOverlay
import org.ppsspp.ppsspp.reader.ui.AnkiSetupActivity
import org.ppsspp.ppsspp.reader.ui.DictionaryManagerActivity
import org.ppsspp.ppsspp.reader.ui.ReaderGamesActivity
import org.ppsspp.ppsspp.reader.ui.ReaderSettingsActivity
import org.ppsspp.ppsspp.reader.ui.ReaderStatsActivity
import org.ppsspp.ppsspp.reader.ui.SecondScreen

/**
 * What ties the reading layer to the emulator's one activity. The activity (Java) calls in
 * here at the few places marked "Reader fork"; everything else of the reader is its own.
 *
 * The emulator draws its menus and the game on one surface, so the reader's layer lies over
 * both: it is shown, and takes touches, only while the game itself is in front.
 */
object ReaderGlue : ReaderNative.Frontend {
    private val main = Handler(Looper.getMainLooper())

    private var activity: Activity? = null
    private var surface: SurfaceView? = null
    private var host: ReaderHost? = null
    private var overlay: ReaderOverlay? = null
    private var second: SecondScreen? = null
    private var resumed = false
    private var inFront = false

    /** How often the emulator is asked whether the game is in front. */
    private const val WATCH_MS = 200L

    private val watch = object : Runnable {
        override fun run() {
            update()
            main.postDelayed(this, WATCH_MS)
        }
    }

    /** The activity has made its surface. */
    @JvmStatic
    fun attach(activity: Activity, surface: View?) {
        detach()
        this.activity = activity
        this.surface = surface as? SurfaceView
        ReaderNative.appContext = activity.applicationContext
        ReaderNative.frontend = this
        ReaderNative.attachOnce()
        StatsSync.saveSyncService = { ReaderNative.saveSyncService()?.let { it[0] to it[1] } }
        HoverCursor.holdSticks = { held ->
            try {
                ReaderNative.holdSticks(held)
            } catch (_: UnsatisfiedLinkError) {
            }
        }

        val secondScreen = SecondScreen(activity) { overlay?.refresh() }
        val readerHost = ReaderHost(activity, secondaryActive = { secondScreen.active })
        secondScreen.host = readerHost
        val layer = ReaderOverlay(activity, readerHost, secondary = false)
        layer.gameSurface = this.surface
        layer.visibility = View.GONE
        activity.addContentView(
            layer,
            ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        host = readerHost
        second = secondScreen
        overlay = layer
        provideFrames()
        main.removeCallbacks(watch)
        main.post(watch)
    }

    @JvmStatic
    fun onResume() {
        resumed = true
        update()
    }

    @JvmStatic
    fun onPause() {
        resumed = false
        update()
    }

    @JvmStatic
    fun onDestroy(activity: Activity) {
        if (this.activity === activity) detach()
    }

    /** A key of a controller or keyboard. True when the emulator must not get it. */
    @JvmStatic
    fun onKey(event: KeyEvent): Boolean = ReaderInput.onKey(event)

    /** A movement of a stick or trigger. True when the emulator must not get it. */
    @JvmStatic
    fun onMotion(event: MotionEvent): Boolean = ReaderInput.onMotion(event)

    private fun detach() {
        main.removeCallbacks(watch)
        second?.stop()
        second = null
        ReaderSession.media?.frameSource = null
        (overlay?.parent as? ViewGroup)?.removeView(overlay)
        overlay = null
        host = null
        surface = null
        activity = null
        inFront = false
    }

    private fun provideFrames() {
        surface?.let { ReaderHost.provideFrames(it) }
    }

    /** Shows the reader's layer while the game is in front and hides it otherwise. */
    private fun update() {
        val front = resumed && ReaderSession.isActive && try {
            ReaderNative.inGame()
        } catch (_: UnsatisfiedLinkError) {
            false
        }
        if (front == inFront) return
        inFront = front
        overlay?.visibility = if (front) View.VISIBLE else View.GONE
        // Nobody is reading a game that is paused or behind a menu.
        ReaderSession.setReading(front)
        if (front) {
            second?.start()
        } else {
            HoverCursor.cancel()
            second?.stop()
        }
    }

    // ---- ReaderNative.Frontend (main thread) ----

    override fun onGameStarted(gameId: String, title: String) {
        val context = activity ?: return
        ReaderSession.start(context, gameId, title)
        provideFrames()
        inFront = false
        update()
    }

    override fun onGameStopped() {
        ReaderSession.stop()
        update()
    }

    override fun onMenu(what: String) {
        val context = activity ?: return
        when (what) {
            "settings" -> context.startActivity(Intent(context, ReaderSettingsActivity::class.java))
            "stats" -> context.startActivity(Intent(context, ReaderStatsActivity::class.java))
            "games" -> context.startActivity(Intent(context, ReaderGamesActivity::class.java))
            "dictionaries" -> context.startActivity(Intent(context, DictionaryManagerActivity::class.java))
            "anki" -> context.startActivity(Intent(context, AnkiSetupActivity::class.java))
            else -> {
                val anchor = overlay ?: return
                if (ReaderSession.isActive) {
                    host?.showMenu(anchor)
                } else {
                    context.startActivity(Intent(context, ReaderSettingsActivity::class.java))
                }
            }
        }
    }
}
