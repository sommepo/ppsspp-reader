// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.TypedValue
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs
import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.screen.Area
import org.ppsspp.ppsspp.reader.screen.PlacedText
import org.ppsspp.ppsspp.reader.screen.ScreenGeometry
import org.ppsspp.ppsspp.reader.screen.ScreenText

/**
 * The invisible layer over the game's picture on one display. Where the game shows text in a
 * text box, and its profile says where the characters are, this makes them something to tap:
 * a tap on a character is reported for a lookup, and no touch that begins inside a text box
 * ever reaches the game. Everywhere else it takes nothing.
 *
 * It draws nothing but a mark on the word being looked up (and, as a setting for checking
 * positions, outlines of what can be tapped).
 */
@SuppressLint("ViewConstructor")
class ScreenTextLayer(
    context: Context,
    /** True for the layer on the second display of a device with two, which is the trackpad. */
    private val secondary: Boolean
) : View(context) {

    interface Listener {
        /** A character of the game's text was tapped. */
        fun onCharacterTap(text: PlacedText, character: PlacedText.Glyph)

        /** A text box was tapped where there is no character. */
        fun onBoxTap(text: PlacedText)
    }

    var listener: Listener? = null

    /** The view the game is drawn in. Positions the game reports are relative to it. */
    var surface: View? = null
        set(value) {
            field = value
            refresh()
        }

    /**
     * What this display shows: the game's picture, where it is on [surface], or the trackpad
     * all over the second display. Replaceable for tests.
     */
    var geometrySource: () -> ScreenGeometry? = {
        if (secondary) {
            ScreenGeometry.padOnly(width, height)
        } else {
            try {
                ScreenGeometry.fromPicture(ReaderNative.pictureRect())
            } catch (_: UnsatisfiedLinkError) {
                null
            }
        }
    }

    /** What is on screen. Replaceable for tests. */
    var textSource: () -> Collection<ScreenText> = { ReaderSession.onScreen }

    /** Whether tapping the game's text is switched on at all. */
    var active: Boolean = true
        set(value) {
            if (field != value) {
                field = value
                refresh()
            }
        }

    /** Draw what can be tapped, to check that it sits on the game's text. */
    var outline: Boolean = false
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    private var placed: List<PlacedText> = emptyList()

    private var marked: PresentedLine? = null
    private var markedRange: IntRange? = null

    /** Where the word being looked up is marked: the only thing this layer normally draws. */
    var marks: List<Area> = emptyList()
        private set

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var down: PlacedText.Hit? = null
    private var downX = 0f
    private var downY = 0f
    private var moved = false

    private val markPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = ReaderStyle.HIGHLIGHT
    }
    private val boxPaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(1.5f)
        color = 0xCC4FC3F7.toInt()
    }
    private val cellPaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(1f)
        color = 0xCCFF5252.toInt()
    }

    /** The texts that can be tapped on this display right now. */
    val texts: List<PlacedText> get() = placed

    /**
     * Brings the layer up to date with what the session says is on screen and with where the
     * picture is drawn. Cheap; called whenever either may have changed.
     */
    fun refresh() {
        val onScreen = if (active) textSource() else emptyList()
        if (onScreen.isEmpty()) {
            if (placed.isNotEmpty()) {
                placed = emptyList()
                updateMarks()
            }
            return
        }
        val geometry = geometry()
        placed = if (geometry == null) {
            emptyList()
        } else {
            onScreen.flatMap { PlacedText.place(it.presented, it.layout, geometry) }
        }
        updateMarks()
    }

    /** What this display shows right now, in this layer's coordinates. */
    private fun geometry(): ScreenGeometry? {
        val geometry = geometrySource() ?: return null
        var offsetX = 0f
        var offsetY = 0f
        surface?.let { view ->
            val own = IntArray(2)
            val other = IntArray(2)
            getLocationInWindow(own)
            view.getLocationInWindow(other)
            offsetX = (other[0] - own[0]).toFloat()
            offsetY = (other[1] - own[1]).toFloat()
        }
        return geometry.movedBy(offsetX, offsetY)
    }

    /**
     * Where this display shows the game's picture ([ScreenLayout.SCREEN_GAME]) or is the
     * trackpad ([ScreenLayout.SCREEN_PAD]), in this layer's coordinates. Empty when it does
     * not.
     */
    fun screenAreas(screen: Int): List<Area> = geometry()?.areasOf(screen).orEmpty()

    /** The character of the game's text at a point of this layer, or null. */
    fun characterAt(x: Float, y: Float): PlacedText.Hit.OnCharacter? =
        hitAt(x, y) as? PlacedText.Hit.OnCharacter

    /** True where a touch belongs to a text box of the game rather than to the game. */
    fun isTextBox(x: Float, y: Float): Boolean = hitAt(x, y) !is PlacedText.Hit.Outside

    /** Marks [range] of [presented]'s text in the game's picture; null takes the mark away. */
    fun mark(presented: PresentedLine, range: IntRange?) {
        marked = if (range == null) null else presented
        markedRange = range
        updateMarks()
    }

    /** A lookup target for [presented] as shown in the game's picture. */
    fun targetFor(presented: PresentedLine): LookupLayer.Target = object : LookupLayer.Target {
        override fun setHighlight(range: IntRange?) = mark(presented, range)
    }

    private fun updateMarks() {
        val line = marked
        val range = markedRange
        marks = if (line == null || range == null) {
            emptyList()
        } else {
            placed.filter { it.presented.backlogId == line.backlogId }.flatMap { it.marks(range) }
        }
        invalidate()
    }

    private fun hitAt(x: Float, y: Float): PlacedText.Hit {
        var box: PlacedText.Hit? = null
        for (text in placed) {
            when (val hit = text.hit(x, y)) {
                is PlacedText.Hit.OnCharacter -> return hit
                is PlacedText.Hit.Box -> if (box == null) box = hit
                PlacedText.Hit.Outside -> {}
            }
        }
        return box ?: PlacedText.Hit.Outside
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // The picture may have moved since the last look.
                refresh()
                val hit = hitAt(event.x, event.y)
                if (hit is PlacedText.Hit.Outside) {
                    down = null
                    return false
                }
                down = hit
                downX = event.x
                downY = event.y
                moved = false
            }

            MotionEvent.ACTION_MOVE ->
                if (abs(event.x - downX) > touchSlop || abs(event.y - downY) > touchSlop) moved = true

            MotionEvent.ACTION_UP -> {
                val hit = down
                down = null
                if (!moved) {
                    when (hit) {
                        is PlacedText.Hit.OnCharacter -> listener?.onCharacterTap(hit.text, hit.character)
                        is PlacedText.Hit.Box -> listener?.onBoxTap(hit.text)
                        else -> {}
                    }
                }
            }

            MotionEvent.ACTION_CANCEL -> down = null
        }
        return down != null || event.actionMasked == MotionEvent.ACTION_UP ||
            event.actionMasked == MotionEvent.ACTION_CANCEL
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        refresh()
    }

    override fun onDraw(canvas: Canvas) {
        if (outline) {
            for (text in placed) {
                canvas.drawRect(text.box.left, text.box.top, text.box.right, text.box.bottom, boxPaint)
                for (character in text.characters) {
                    val cell = character.drawn
                    canvas.drawRect(cell.left, cell.top, cell.right, cell.bottom, cellPaint)
                }
            }
        }
        val radius = dp(3f)
        for (mark in marks) {
            canvas.drawRoundRect(mark.left, mark.top, mark.right, mark.bottom, radius, radius, markPaint)
        }
    }

    private fun dp(value: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)
}
