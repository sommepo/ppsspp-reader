// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.widget.LinearLayout
import android.widget.TextView
import androidx.preference.PreferenceManager

/**
 * The console's buttons for a game played by touch: every button of its controller, in two short
 * columns at the right edge of the screen (the d-pad and the small buttons on the left, the face
 * buttons and START on the right), shown only while Play by touch is on and such a game runs. A
 * finger held on a button holds the game's button. A tab above them tucks them away and brings
 * them back; that choice is kept (it was asked for on the Fold, 2026-10-16, where a game's own
 * subset of buttons, chosen from its script, had left a menu that could not be answered).
 */
@SuppressLint("ViewConstructor")
class TouchButtons(
    context: Context,
    /** Presses (true) or releases (false) a button of the console, by its number. */
    private val press: (code: Int, down: Boolean) -> Unit
) : LinearLayout(context) {

    /** One of the console's buttons: its number for [press], what the button shows. */
    data class Button(val code: Int, val label: String, val colour: Int)

    private var shownNames: List<String> = emptyList()
    private val preferences = PreferenceManager.getDefaultSharedPreferences(context)
    private val tab = TextView(context)
    private val grid = LinearLayout(context)

    init {
        orientation = VERTICAL
        gravity = Gravity.END
        visibility = GONE
        tab.apply {
            tag = TAB
            gravity = Gravity.CENTER
            includeFontPadding = false
            setTextColor(TAB_INK)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            typeface = Typeface.DEFAULT_BOLD
            background = GradientDrawable().apply {
                cornerRadius = dp(TAB_DP / 2f).toFloat()
                setColor(FILL)
                setStroke(dp(1f), EDGE)
            }
            alpha = REST_ALPHA
            setOnClickListener { tucked = !tucked }
        }
        addView(tab, LayoutParams(dp(TAB_DP * 1.4f), dp(TAB_DP)).apply { setMargins(0, 0, 0, dp(GAP_DP)) })
        grid.orientation = HORIZONTAL
        addView(grid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        showTucked()
    }

    /** The buttons are tucked away behind the tab (kept between starts). */
    var tucked: Boolean
        get() = preferences.getBoolean(KEY_TUCKED, false)
        set(value) {
            preferences.edit().putBoolean(KEY_TUCKED, value).apply()
            showTucked()
        }

    private fun showTucked() {
        val away = tucked
        grid.visibility = if (away) GONE else VISIBLE
        // ‹ brings them out from the edge, › puts them back to it.
        tab.text = if (away) "‹" else "›"
        tab.contentDescription = if (away) "Show the buttons" else "Hide the buttons"
    }

    /** Shows the buttons of these names (unknown names are passed over), or none. */
    fun show(names: List<String>) {
        val known = names.filter { it in BUTTONS }
        if (known == shownNames) return
        shownNames = known
        grid.removeAllViews()
        val (left, right) = known.partition { it in LEFT }
        for (column in listOf(left, right)) {
            if (column.isEmpty()) continue
            val stack = LinearLayout(context).apply { orientation = VERTICAL }
            for (name in column) {
                stack.addView(round(name, BUTTONS.getValue(name)), LayoutParams(dp(SIZE_DP), dp(SIZE_DP)).apply {
                    setMargins(dp(GAP_DP / 2), dp(GAP_DP / 2), dp(GAP_DP / 2), dp(GAP_DP / 2))
                })
            }
            grid.addView(stack, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        }
        visibility = if (known.isEmpty()) GONE else VISIBLE
    }

    /** The names shown, for tests (also while tucked away). */
    val names: List<String> get() = shownNames

    /** The buttons can be seen and touched (shown, and not tucked away). */
    val buttonsShown: Boolean get() = visibility == VISIBLE && grid.visibility == VISIBLE

    @SuppressLint("ClickableViewAccessibility")
    private fun round(name: String, button: Button) = TextView(context).apply {
        text = button.label
        tag = name
        contentDescription = name
        gravity = Gravity.CENTER
        includeFontPadding = false
        setTextColor(button.colour)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, if (button.label.length > 2) 9f else if (button.label.length > 1) 13f else 22f)
        typeface = Typeface.DEFAULT_BOLD
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(FILL)
            setStroke(dp(1.5f), EDGE)
        }
        alpha = REST_ALPHA
        setOnTouchListener { view, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    view.alpha = 1f
                    press(button.code, true)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    view.alpha = REST_ALPHA
                    press(button.code, false)
                }
            }
            true
        }
    }

    private fun dp(value: Float) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics).toInt()

    companion object {
        private const val SIZE_DP = 42f
        private const val GAP_DP = 8f
        private const val TAB_DP = 26f
        private const val REST_ALPHA = 0.55f
        private const val FILL = 0xCC1A1F27.toInt()
        private const val EDGE = 0x99FFFFFF.toInt()

        /** The preference that keeps the buttons tucked away. */
        const val KEY_TUCKED = "reader_touch_buttons_tucked"

        /** The tab's tag, for tests. */
        const val TAB = "tab"

        private const val TAB_INK = 0xFFE8ECF2.toInt()

        /** Every button, as the column shows them for a game played by touch. */
        val ALL: List<String> = listOf("up", "down", "left", "right", "select", "l", "triangle", "circle", "cross", "square", "start", "r")

        /** The buttons of the left column (the rest are on the right). */
        private val LEFT: Set<String> = setOf("up", "down", "left", "right", "select", "l")

        /**
         * The PSP's buttons by the names scripts use, each with its bit in the button word (what
         * [org.ppsspp.ppsspp.reader.ReaderNative.pressButton] takes) and the colours of its sign.
         */
        val BUTTONS: Map<String, Button> = mapOf(
            "up" to Button(4, "▲", 0xFFE8ECF2.toInt()),
            "down" to Button(6, "▼", 0xFFE8ECF2.toInt()),
            "left" to Button(7, "◀", 0xFFE8ECF2.toInt()),
            "right" to Button(5, "▶", 0xFFE8ECF2.toInt()),
            "circle" to Button(13, "○", 0xFFFF6B6B.toInt()),
            "cross" to Button(14, "×", 0xFF7DA7FF.toInt()),
            "square" to Button(15, "□", 0xFFFF8FD8.toInt()),
            "triangle" to Button(12, "△", 0xFF6EE7A8.toInt()),
            "start" to Button(3, "START", 0xFFE8ECF2.toInt()),
            "select" to Button(0, "SELECT", 0xFFE8ECF2.toInt()),
            "l" to Button(8, "L", 0xFFE8ECF2.toInt()),
            "r" to Button(9, "R", 0xFFE8ECF2.toInt()),
        )
    }
}
