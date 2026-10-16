// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import org.ppsspp.ppsspp.reader.ReaderCatalog
import org.ppsspp.ppsspp.reader.ReaderSettings
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
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
 * ever reaches the game. Everywhere else it takes nothing, unless the game is played by touch
 * ([touchPlay]): then a tap on the picture is the player going on, and a tap on an answer of a
 * choice picks it (held, it is a lookup), for a game that does not take touches itself.
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

        /** Touch play: the picture was tapped where there is no text, or a text box beside it. */
        fun onPictureTap() {}

        /** Touch play: answer [answer] (from 0) of the choice [text] was tapped. */
        fun onAnswerTap(text: PlacedText, answer: Int) {}
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

    /** Touch play is switched on (it applies to games whose profile says how: [touchMode]). */
    var touchPlay: Boolean = false

    /**
     * How the running game is played by touch, as its profile says: ReaderCatalog.TOUCH_GAME, it
     * takes touches itself, so the taps on its picture and on its answers go to it as they are;
     * TOUCH_PRESS, the taps are turned into its buttons; empty, it is not played by touch.
     */
    var touchMode: () -> String = { "" }

    private val playing get() = touchPlay && touchMode().isNotEmpty()
    private fun gameTakesTouch() = touchMode() == ReaderCatalog.TOUCH_GAME

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
    private var downTime = 0L
    private var moved = false

    /** Touch play: the touch going on began on the picture, beside any text. */
    private var downOnPicture = false
    private val longPress = ViewConfiguration.getLongPressTimeout().toLong()

    private val markPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = ReaderStyle.HIGHLIGHT
    }
    /** How the looked-up word is marked: one of ReaderSettings.WORD_MARK_…. */
    var markStyle: String = ReaderSettings.WORD_MARK_OUTLINE
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }
    private val frameWidth get() = dp(2f)
    private val framePaint by lazy {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = frameWidth
            color = (ReaderStyle.HIGHLIGHT and 0x00FFFFFF) or 0xE6000000.toInt()
        }
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
        updateMarks(toAnotherWord = true)
    }

    /** A lookup target for [presented] as shown in the game's picture. */
    fun targetFor(presented: PresentedLine): LookupLayer.Target = object : LookupLayer.Target {
        override fun setHighlight(range: IntRange?) = mark(presented, range)
    }

    /** The mark as drawn, on its way to [marks]. */
    private val glide = MarkGlide(this)

    /** [toAnotherWord]: the mark glides there; else the picture moved and it simply follows. */
    private fun updateMarks(toAnotherWord: Boolean = false) {
        val line = marked
        val range = markedRange
        val before = marks
        marks = if (line == null || range == null) {
            emptyList()
        } else {
            placed.filter { it.presented.backlogId == line.backlogId }.flatMap { it.marks(range) }
        }
        if (marks != before || toAnotherWord) {
            glide.setTarget(marks.map { RectF(it.left, it.top, it.right, it.bottom) },
                toAnotherWord || glide.moving)
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
                downOnPicture = false
                val toGame = hit is PlacedText.Hit.Outside ||
                    (playing && gameTakesTouch() && isAnswer(hit))
                if (toGame) {
                    down = null
                    if (!playing || gameTakesTouch() || hit !is PlacedText.Hit.Outside) return false
                    // While a choice is up a tap beside it would confirm whatever answer is lit:
                    // it is taken and does nothing (an answer is picked by tapping it).
                    downOnPicture = placed.none { it.presented.line.isChoice }
                    if (!downOnPicture) {
                        down = PlacedText.Hit.Outside
                        downX = event.x
                        downY = event.y
                        downTime = event.eventTime
                        moved = false
                        return true
                    }
                } else {
                    down = hit
                }
                downX = event.x
                downY = event.y
                downTime = event.eventTime
                moved = false
            }

            MotionEvent.ACTION_MOVE ->
                if (abs(event.x - downX) > touchSlop || abs(event.y - downY) > touchSlop) moved = true

            MotionEvent.ACTION_UP -> {
                val hit = down
                val onPicture = downOnPicture
                down = null
                downOnPicture = false
                if (!moved) {
                    val held = event.eventTime - downTime >= longPress
                    when {
                        onPicture -> listener?.onPictureTap()
                        hit is PlacedText.Hit.OnCharacter && playing && !gameTakesTouch() && isAnswer(hit) && !held ->
                            listener?.onAnswerTap(hit.text, answerAt(hit.text, hit.character))
                        hit is PlacedText.Hit.OnCharacter -> listener?.onCharacterTap(hit.text, hit.character)
                        // Beside the answers of a choice a tap would confirm whatever is lit: not that.
                        hit is PlacedText.Hit.Box && playing && !gameTakesTouch() && !hit.text.presented.line.isChoice ->
                            listener?.onPictureTap()
                        hit is PlacedText.Hit.Box -> listener?.onBoxTap(hit.text)
                        else -> {}
                    }
                }
            }

            MotionEvent.ACTION_CANCEL -> {
                down = null
                downOnPicture = false
            }
        }
        return down != null || downOnPicture || event.actionMasked == MotionEvent.ACTION_UP ||
            event.actionMasked == MotionEvent.ACTION_CANCEL
    }

    private fun isAnswer(hit: PlacedText.Hit): Boolean =
        hit is PlacedText.Hit.OnCharacter && hit.text.presented.line.isChoice

    /** Which answer of a choice a character is in: the answers are its text's lines. */
    private fun answerAt(text: PlacedText, character: PlacedText.Glyph): Int {
        val line = text.presented.line.text
        val end = character.offset.coerceIn(0, line.length)
        return line.subSequence(0, end).count { it == '\n' }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        refresh()
    }

    override fun onDraw(canvas: Canvas) {
        glide.frame()
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
        val alpha = (glide.opacity * 255).toInt()
        when (markStyle) {
            ReaderSettings.WORD_MARK_NONE -> Unit
            ReaderSettings.WORD_MARK_OUTLINE -> {
                val inset = -frameWidth / 2
                framePaint.alpha = alpha * 0xE6 / 255
                for (mark in glide.drawn) {
                    canvas.drawRoundRect(mark.left + inset, mark.top + inset, mark.right - inset,
                        mark.bottom - inset, radius, radius, framePaint)
                }
            }
            else -> {
                markPaint.alpha = alpha * (ReaderStyle.HIGHLIGHT ushr 24) / 255
                for (mark in glide.drawn) {
                    canvas.drawRoundRect(mark.left, mark.top, mark.right, mark.bottom, radius, radius, markPaint)
                }
            }
        }
    }

    private fun dp(value: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)
}
