// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.app.Activity
import android.app.Presentation
import android.content.Context
import android.content.SharedPreferences
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import org.ppsspp.ppsspp.reader.ReaderLog as Log
import org.ppsspp.ppsspp.reader.ReaderSettings

/**
 * The reader's use of a second display. The console has one screen, so on a device with two
 * displays the other one has nothing to show: while a game runs, the reader makes it the
 * trackpad for its cursor and the place for its panel. With one display, or with this switched
 * off, there is no such screen and the game's own display does both.
 *
 * The window on the second display never takes the focus, so the controller keeps talking to
 * the game.
 */
class SecondScreen(
    private val activity: Activity,
    /** Called when the second screen appeared or went away. */
    private val onChanged: () -> Unit
) : DisplayManager.DisplayListener, SharedPreferences.OnSharedPreferenceChangeListener {

    private val displays = activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val settings = ReaderSettings(activity)
    private var pad: Pad? = null
    private var started = false

    /** What the overlay on the second display needs; set before [start]. */
    var host: ReaderOverlay.Host? = null

    /** True while the second display is the reader's. */
    val active: Boolean get() = pad != null

    fun start() {
        if (started) return
        started = true
        displays.registerDisplayListener(this, Handler(Looper.getMainLooper()))
        settings.preferences.registerOnSharedPreferenceChangeListener(this)
        update()
    }

    fun stop() {
        if (!started) return
        started = false
        displays.unregisterDisplayListener(this)
        settings.preferences.unregisterOnSharedPreferenceChangeListener(this)
        release()
    }

    override fun onDisplayAdded(displayId: Int) = update()

    override fun onDisplayRemoved(displayId: Int) = update()

    override fun onDisplayChanged(displayId: Int) = update()

    override fun onSharedPreferenceChanged(prefs: SharedPreferences?, key: String?) {
        if (key == null || key == ReaderSettings.KEY_SECOND_SCREEN) update()
    }

    /** The display to use: one that is not the game's, is switched on and can take a window. */
    private fun choose(): Display? {
        if (!settings.secondScreen) return null
        val own = activity.display?.displayId ?: Display.DEFAULT_DISPLAY
        val presentable = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        val candidates = displays.displays.filter { display ->
            display.displayId != own && display.isValid && display.state != Display.STATE_OFF &&
                (display.displayId != Display.DEFAULT_DISPLAY || presentable.any { it.displayId == display.displayId }) &&
                display.flags and Display.FLAG_PRIVATE == 0
        }
        // Some devices keep a virtual display named like the built-in one: a real second
        // display, named differently, goes first.
        val main = displays.getDisplay(Display.DEFAULT_DISPLAY)
        return candidates.firstOrNull { it.name != main?.name } ?: candidates.firstOrNull()
    }

    private fun update() {
        if (!started || activity.isFinishing || activity.isDestroyed) return
        val wanted = choose()
        val current = pad
        if (current?.display?.displayId == wanted?.displayId) return
        release(notify = false)
        val host = host
        if (wanted != null && host != null) {
            try {
                pad = Pad(activity, wanted, host).also { it.show() }
            } catch (e: WindowManager.InvalidDisplayException) {
                // The display went away meanwhile; the listener hears of it.
                Log.warning("[Reader] The second display could not be used: ${e.message}")
                pad = null
            } catch (e: WindowManager.BadTokenException) {
                Log.warning("[Reader] The second display could not be used: ${e.message}")
                pad = null
            }
        }
        onChanged()
    }

    private fun release(notify: Boolean = true) {
        val current = pad ?: return
        pad = null
        try {
            current.dismiss()
        } catch (_: Exception) {
            // Its display is gone, and the window with it.
        }
        if (notify) onChanged()
    }

    /** The window on the second display: black, with the reader's overlay for that display. */
    private class Pad(context: Context, display: Display, private val host: ReaderOverlay.Host) :
        Presentation(context, display) {

        override fun onCreate(savedInstanceState: Bundle?) {
            super.onCreate(savedInstanceState)
            window?.setFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            )
            val root = FrameLayout(context)
            root.setBackgroundColor(Color.BLACK)
            root.addView(
                ReaderOverlay(context, host, secondary = true),
                FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            )
            setContentView(root)
        }
    }
}
