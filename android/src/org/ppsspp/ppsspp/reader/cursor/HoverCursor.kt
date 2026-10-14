// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.cursor

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.hypot
import kotlin.math.pow
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderSession

/**
 * The cursor of the game's screen. While its button is held, the second display (or, without
 * one, the game's screen itself) is a trackpad and the sticks steer: the cursor moves over the
 * game's picture, and a word it rests on is looked up, so that a word can be reached without a
 * finger on the game. As a setting, the button switches the cursor on and off instead
 * ([toggles]).
 *
 * On a device with two displays none of that is needed: the second display is the trackpad
 * for as long as there is text to point at ([always]). The cursor is moved by the finger
 * alone then: the button, the sticks and the d-pad are the game's, all of them, all the time.
 *
 * This object holds the state: who holds the button, where the cursor is, what the sticks say.
 * The overlays of the displays attach as [Surface]s: they show it and feed it touches. The
 * emulator asks about the cursor's button and passes the sticks through [onButton] and
 * [onStick] before the game gets them; what is taken here does not reach the game. Main
 * thread throughout.
 */
object HoverCursor {
    /** An overlay taking part. */
    interface Surface {
        /** Whether the cursor is in use, or where it is, changed. */
        fun onCursorChanged()

        /** The trackpad was tapped: a click where the cursor is. For the main screen to act on. */
        fun onCursorClick() {}

        /** True when this surface is the second display's, which is the trackpad. */
        fun hasTrackpad(): Boolean

        /** True when this surface is a display of its own beside the game's, all trackpad. */
        fun isSecondDisplay(): Boolean = false
    }

    /** Where a press of the button came from. A button can be held from more than one place. */
    const val SOURCE_KEY = 0
    const val SOURCE_AXIS = 1
    const val SOURCE_TOUCH = 2
    const val SOURCE_HOTKEY = 3

    /** Whether there is anything to point at: a game is running and its text is being read. */
    var armed: () -> Boolean = { ReaderSession.isActive && ReaderSession.profileStatus.loaded }

    /**
     * Takes the sticks away from the game, which sees them at rest from then on, or hands them
     * back as they are. While they are taken, their movement arrives at [onStick].
     */
    var holdSticks: (held: Boolean) -> Unit = { held ->
        try {
            ReaderNative.holdSticks(held)
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
    }

    /** Tells the emulator which button to ask about (see [onButton]). */
    var watchButton: (button: Int) -> Unit = { }

    /** No button holds the cursor. */
    const val BUTTON_NONE = -1

    /** L2, as Android numbers a controller's buttons (KeyEvent.KEYCODE_BUTTON_L2). */
    const val BUTTON_L2 = 104

    /** The emulated button that holds the cursor (an Android key code), or [BUTTON_NONE]. */
    var button = BUTTON_L2
        private set

    /** How fast the cursor moves, in percent. */
    var speedPercent = 100
        private set

    /**
     * The button switches the cursor on with one press and off with the next, rather than
     * having to be held.
     */
    var toggles = false
        private set

    private val surfaces = CopyOnWriteArrayList<Surface>()

    /** Where the button is down right now (see the SOURCE constants). */
    private val holders = HashSet<Int>()

    /** With [toggles]: a press has switched the cursor on. */
    private var switchedOn = false
    private val sticks = HashMap<Int, FloatArray>()
    private val main = Handler(Looper.getMainLooper())
    private var lastStep = 0L
    private var stepping = false

    /**
     * A second display is the trackpad and there is something to point at: the cursor is in
     * use without its button, and nothing of the controller is taken from the game.
     */
    @Volatile
    var always = false
        private set

    /** True while the cursor is in use: its button is held, or has switched it on, or [always]. */
    val active: Boolean get() = always || if (toggles) switchedOn else holders.isNotEmpty()

    /** True while the sticks steer the cursor and are kept from the game. */
    val takesSticks: Boolean get() = active && !always

    /**
     * Looks again at whether a second display is the trackpad and whether there is anything
     * to point at. The overlays call it when either may have changed.
     */
    fun refresh() {
        val now = surfaces.any { it.isSecondDisplay() } && armed()
        if (now == always) return
        if (now) {
            // Whoever held the button lets go first: from here on the sticks are the game's.
            if (active) cancel()
            always = true
            everywhere = false
            changed()
        } else {
            always = false
            if (!active) released() else changed()
        }
    }

    /** Where the cursor is, in pixels of the main screen's overlay. */
    var x = 0f
        private set
    var y = 0f
        private set

    /** The line the cursor was last brought to the start of (see [home]), or null. */
    var homedFor: Long? = null
        private set

    /** The size of the main screen's overlay: the cursor stays inside it. */
    var width = 0
        private set
    var height = 0
        private set

    /**
     * There is no second display to be the trackpad: the whole game screen is the trackpad
     * then, and is not dimmed, as there would be nothing left to read.
     */
    var everywhere = false
        private set

    fun configure(button: Int, speedPercent: Int, toggles: Boolean = false) {
        if ((button != this.button || toggles != this.toggles) && active) cancel()
        this.button = button
        this.speedPercent = speedPercent.coerceIn(10, 500)
        this.toggles = toggles
        watchButton(button)
    }

    fun attach(surface: Surface) {
        surfaces.addIfAbsent(surface)
    }

    fun detach(surface: Surface) {
        surfaces.remove(surface)
        refresh()
    }

    /** The main screen's overlay says how large it is. */
    fun setBounds(width: Int, height: Int) {
        this.width = width
        this.height = height
        x = x.coerceIn(0f, maxX())
        y = y.coerceIn(0f, maxY())
    }

    /**
     * An emulated button was pressed or released. True when it is the cursor's button and
     * there is something to point at: the game must not get it then.
     */
    fun onButton(button: Int, pressed: Boolean, source: Int): Boolean {
        if (this.button == BUTTON_NONE || button != this.button) return false
        if (always) return false // the trackpad is there anyway: the button is the game's
        if (!armed()) {
            if (active) cancel()
            return false
        }
        hold(source, pressed)
        return true
    }

    /** The key bound to the cursor in the emulator's hotkey settings went down or up. */
    fun onHotkey(pressed: Boolean) {
        if (always) return
        if (pressed && !armed()) {
            if (active) cancel()
            return
        }
        hold(SOURCE_HOTKEY, pressed)
    }

    /**
     * A stick moved to ([x], [y]), each from -1 to 1, right and down being positive. True while
     * the cursor is in use: the movement is the cursor's and must not reach the game.
     */
    fun onStick(stick: Int, x: Float, y: Float): Boolean {
        if (!takesSticks) return false
        sticks[stick] = floatArrayOf(x, y)
        if (!stepping && deflection() >= DEAD_ZONE) {
            stepping = true
            lastStep = SystemClock.uptimeMillis()
            main.postDelayed(step, FRAME_MS)
        }
        return true
    }

    /** Moves the cursor by a distance in pixels of the main screen's overlay. */
    fun moveBy(dx: Float, dy: Float) {
        val nextX = (x + dx).coerceIn(0f, maxX())
        val nextY = (y + dy).coerceIn(0f, maxY())
        if (nextX == x && nextY == y) return
        x = nextX
        y = nextY
        changed()
    }

    /** Puts the cursor at the start of line [line]'s text, where reading begins. */
    fun home(x: Float, y: Float, line: Long?) {
        this.x = x.coerceIn(0f, maxX())
        this.y = y.coerceIn(0f, maxY())
        homedFor = line
        changed()
    }

    /** A tap on the trackpad. */
    fun click() {
        if (active) surfaces.forEach { it.onCursorClick() }
    }

    /** Lets go of the cursor whoever holds it: the game was paused, or closed. */
    fun cancel() {
        val was = active
        holders.clear()
        switchedOn = false
        if (was) released()
    }

    private fun hold(source: Int, pressed: Boolean) {
        val was = active
        // A press counts once: a held key repeats, and a trigger's axis reports its state
        // with every movement of a stick.
        val firstPress = pressed && holders.add(source)
        if (!pressed) holders.remove(source)
        if (toggles && firstPress) switchedOn = !switchedOn
        if (active == was) return
        if (active) {
            sticks.clear()
            holdSticks(true)
            everywhere = surfaces.none { it.hasTrackpad() }
            changed()
        } else {
            released()
        }
    }

    private fun released() {
        holdSticks(false)
        sticks.clear()
        stepping = false
        main.removeCallbacks(step)
        changed()
    }

    private fun changed() = surfaces.forEach { it.onCursorChanged() }

    private fun maxX() = (width - 1).coerceAtLeast(0).toFloat()

    private fun maxY() = (height - 1).coerceAtLeast(0).toFloat()

    /** How far the sticks are pushed, all of them together. */
    private fun deflection(): Float {
        var sx = 0f
        var sy = 0f
        for (stick in sticks.values) {
            sx += stick[0]
            sy += stick[1]
        }
        return hypot(sx, sy)
    }

    /** Moves the cursor as the sticks say, frame by frame, for as long as one is pushed. */
    private val step = object : Runnable {
        override fun run() {
            if (!active || !stepping) return
            val now = SystemClock.uptimeMillis()
            val seconds = (now - lastStep).coerceIn(0L, MAX_STEP_MS) / 1000f
            lastStep = now
            var sx = 0f
            var sy = 0f
            for (stick in sticks.values) {
                sx += stick[0]
                sy += stick[1]
            }
            val push = hypot(sx, sy)
            if (push < DEAD_ZONE) {
                stepping = false
                return
            }
            // Slow near the centre, for the last few pixels onto a character; fast at the rim.
            val speed = minOf(push, 1f).pow(CURVE) * fullSpeed()
            moveBy(sx / push * speed * seconds, sy / push * speed * seconds)
            main.postDelayed(this, FRAME_MS)
        }
    }

    /** Pixels a second with a stick pushed all the way: across the main screen in a second. */
    private fun fullSpeed(): Float = width * speedPercent / 100f / CROSSING_SECONDS

    private const val DEAD_ZONE = 0.12f
    private const val CURVE = 1.7f
    private const val CROSSING_SECONDS = 1.0f
    private const val FRAME_MS = 16L
    private const val MAX_STEP_MS = 50L

    /** Back to the state before anything was pressed, for tests. */
    fun reset() {
        holders.clear()
        switchedOn = false
        always = false
        toggles = false
        sticks.clear()
        stepping = false
        main.removeCallbacks(step)
        surfaces.clear()
        x = 0f
        y = 0f
        homedFor = null
        everywhere = false
        button = BUTTON_L2
        speedPercent = 100
    }
}
