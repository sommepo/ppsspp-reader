// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.os.SystemClock
import android.util.TypedValue
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import org.ppsspp.ppsspp.R

/**
 * What a card asks before it is made, in a game whose cards ask about their audio: whether it
 * gets what the game played during its line. One line of text and three boxed buttons, in the
 * dictionary popup's look: listen to the clip, make the card with it, make the card without it.
 */
@SuppressLint("ViewConstructor")
class AudioPromptView(context: Context) : LinearLayout(context) {
    var onListen: (() -> Unit)? = null
    var onKeep: (() -> Unit)? = null
    var onDrop: (() -> Unit)? = null

    private val question = TextView(context)
    private val listen = button(R.string.reader_audio_listen, ReaderStyle.POPUP_BUTTON_TEXT) { onListen?.invoke() }
    private val keep = button(R.string.reader_audio_keep, ReaderStyle.POPUP_BUTTON_KEEP) { onKeep?.invoke() }
    private val drop = button(R.string.reader_audio_drop, ReaderStyle.POPUP_BUTTON_TEXT) { onDrop?.invoke() }
    private var shownAt = 0L

    init {
        orientation = VERTICAL
        background = ConsoleLook.ground(resources.displayMetrics.density)
        elevation = dp(8f)
        isClickable = true // taps between the buttons answer nothing
        question.setText(R.string.reader_audio_question)
        question.setTextColor(ReaderStyle.POPUP_TEXT)
        question.includeFontPadding = false
        addView(question, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        val row = LinearLayout(context).apply { orientation = HORIZONTAL }
        for (button in listOf(listen, keep, drop)) {
            row.addView(button, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        }
        addView(row, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        prepare(100)
    }

    /**
     * Makes the question ready to be shown, its text at [scale] percent of the normal size (the
     * dictionary popup's setting).
     */
    fun prepare(scale: Int) {
        val factor = scale.coerceIn(50, 250) / 100f
        fun size(view: TextView) = view.setTextSize(
            TypedValue.COMPLEX_UNIT_PX,
            TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, TEXT_SP, resources.displayMetrics) * factor
        )
        val pad = dp(8f * factor).toInt()
        setPadding(pad, pad, pad, pad)
        size(question)
        question.setPadding(dp(2f).toInt(), 0, 0, pad)
        for ((index, button) in listOf(listen, keep, drop).withIndex()) {
            size(button)
            button.setPadding(dp(12f * factor).toInt(), dp(9f * factor).toInt(), dp(12f * factor).toInt(), dp(9f * factor).toInt())
            (button.layoutParams as LayoutParams).marginStart = if (index == 0) 0 else dp(6f * factor).toInt()
        }
        // As wide as the wider of its two labels: the other buttons stay where they are when
        // it changes from one to the other, under a finger that is on its way to them.
        listen.minWidth = listOf(R.string.reader_audio_listen, R.string.reader_audio_stop)
            .maxOf { listen.paint.measureText(context.getString(it)) }.toInt() + 1 +
            listen.paddingLeft + listen.paddingRight
        setPlaying(false)
        shownAt = SystemClock.uptimeMillis()
    }

    /** While the clip plays, the first button stops it. */
    fun setPlaying(playing: Boolean) {
        listen.setText(if (playing) R.string.reader_audio_stop else R.string.reader_audio_listen)
    }

    private fun button(label: Int, colour: Int, action: () -> Unit) = TextView(context).apply {
        setText(label)
        setTextColor(colour)
        gravity = Gravity.CENTER
        includeFontPadding = false
        background = boxBackground(colour)
        isClickable = true
        setOnClickListener {
            // The question comes up where a finger has just been: the press that asked for
            // the card, or its bounce, is not an answer.
            if (SystemClock.uptimeMillis() - shownAt >= GUARD_MS) action()
        }
    }

    private fun boxBackground(colour: Int): Drawable {
        fun shape(fill: Int) = GradientDrawable().apply {
            setColor(fill)
            cornerRadius = dp(4f)
            setStroke(dp(1f).toInt().coerceAtLeast(1), (colour and 0x00FFFFFF) or 0xA0000000.toInt())
        }
        return StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), shape(ReaderStyle.POPUP_BOX_PRESSED))
            addState(intArrayOf(), shape(ReaderStyle.POPUP_BOX))
        }
    }

    private fun dp(value: Float) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)

    companion object {
        private const val TEXT_SP = 14f

        /** For this long after it came up, the question takes no answer. */
        const val GUARD_MS = 300L
    }
}
