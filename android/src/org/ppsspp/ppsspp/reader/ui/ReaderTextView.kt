// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import org.ppsspp.ppsspp.reader.ReaderSettings
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.text.Spannable
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.LineHeightSpan
import android.text.style.ReplacementSpan
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import androidx.appcompat.widget.AppCompatTextView
import kotlin.math.ceil
import kotlin.math.max
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.offsetByCodePointsSafe

/**
 * Shows one line of game text with optional furigana, and reports which character was tapped
 * so a dictionary lookup can start there.
 *
 * Furigana are drawn by replacement spans over their base text; the underlying string is the
 * plain line, so offsets reported to listeners index the line's text directly.
 */
class ReaderTextView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : AppCompatTextView(context, attrs), LookupLayer.Target {

    /** Called with the UTF-16 offset of the tapped character and its bounds in this view. */
    var onCharacterTap: ((offset: Int, bounds: Rect) -> Unit)? = null

    /** Called for a tap that is not on text. */
    var onEmptyTap: (() -> Unit)? = null

    var onLongPress: (() -> Unit)? = null

    private var plainText: String = ""
    private var rubySpans: List<RubySpan> = emptyList()
    private var furigana = true
    /** The looked-up word's range in the line, or null. */
    var markedRange: IntRange? = null
        private set
    private val framePath = android.graphics.Path()
    private val framePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeJoin = Paint.Join.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    /** The mark as drawn, on its way to the word's rows; and the rows it was last sent to. */
    private val glide = MarkGlide(this)
    private var markTargets: List<RectF> = emptyList()
    private var toAnotherWord = false

    var wordHighlightColor: Int = 0x66FFD54F

    private val gestures =
        GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true

            override fun onSingleTapUp(e: MotionEvent): Boolean {
                val offset = characterAt(e.x, e.y)
                if (offset < 0) {
                    onEmptyTap?.invoke()
                } else {
                    onCharacterTap?.invoke(offset, boundsOf(offset))
                }
                return true
            }

            override fun onLongPress(e: MotionEvent) {
                onLongPress?.invoke()
            }
        })

    init {
        includeFontPadding = false
    }

    /** Shows [text] with [ruby] above it when [showFurigana]. */
    fun setLine(text: String, ruby: List<RubySpan>, showFurigana: Boolean) {
        plainText = text
        rubySpans = ruby
        furigana = showFurigana
        markedRange = null
        markTargets = emptyList()
        glide.clear()
        rebuild()
    }

    override fun setTextSize(unit: Int, size: Float) {
        super.setTextSize(unit, size)
        if (plainText.isNotEmpty()) rebuild() // the line-height span depends on the size
    }

    /** Marks [start, end) (UTF-16 offsets of the line) as the looked-up word; null clears it. */
    override fun setHighlight(range: IntRange?) {
        markedRange = range?.takeIf { !it.isEmpty() && it.last < text.length }
        toAnotherWord = true
        invalidate()
    }

    /**
     * The mark is drawn here rather than as a span of the text, so that it can glide from one
     * word to the next: filled behind the text, or framed over it (ReaderSettings.wordMark).
     */
    override fun onDraw(canvas: Canvas) {
        val targets = markedRange?.let { rowsOf(it) }.orEmpty()
        if (targets != markTargets) {
            glide.setTarget(targets, toAnotherWord || glide.moving)
            markTargets = targets
        }
        toAnotherWord = false
        glide.frame()
        val radius = 3f * resources.displayMetrics.density
        val alpha = glide.opacity
        if (markStyle == ReaderSettings.WORD_MARK_HIGHLIGHT && glide.visible) {
            fillPaint.color = wordHighlightColor
            fillPaint.alpha = ((wordHighlightColor ushr 24) * alpha).toInt()
            for (row in glide.drawn) canvas.drawRoundRect(row, radius, radius, fillPaint)
        }
        super.onDraw(canvas)
        if (markStyle == ReaderSettings.WORD_MARK_OUTLINE && glide.visible) {
            val width = 2f * resources.displayMetrics.density
            framePaint.strokeWidth = width
            framePaint.color = (wordHighlightColor and 0x00FFFFFF) or 0xE6000000.toInt()
            framePaint.alpha = (0xE6 * alpha).toInt()
            for (row in glide.drawn) {
                canvas.drawRoundRect(row.left - width / 2, row.top - width / 2, row.right + width / 2,
                    row.bottom + width / 2, radius, radius, framePaint)
            }
        }
    }

    /** The rows [range] takes in the text's layout, in this view's coordinates. */
    private fun rowsOf(range: IntRange): List<RectF> {
        val textLayout = layout ?: return emptyList()
        val end = (range.last + 1).coerceAtMost(text.length)
        if (range.first >= end) return emptyList()
        val dx = totalPaddingLeft.toFloat() - scrollX
        val dy = totalPaddingTop.toFloat() - scrollY
        val rows = mutableListOf<RectF>()
        for (line in textLayout.getLineForOffset(range.first)..textLayout.getLineForOffset(end - 1)) {
            val from = maxOf(range.first, textLayout.getLineStart(line))
            val to = minOf(end, textLayout.getLineEnd(line))
            if (from >= to) continue
            framePath.reset()
            textLayout.getSelectionPath(from, to, framePath)
            val row = RectF()
            @Suppress("DEPRECATION")
            framePath.computeBounds(row, true)
            if (row.isEmpty) continue
            row.offset(dx, dy)
            rows.add(row)
        }
        return rows
    }

    private fun rebuild() {
        val builder = SpannableStringBuilder(plainText)
        if (furigana && rubySpans.isNotEmpty()) {
            for (span in rubySpans) {
                val start = plainText.offsetByCodePointsSafe(0, span.start)
                val end = plainText.offsetByCodePointsSafe(start, span.length)
                // A replacement span cannot cross a line break.
                if (end <= start || plainText.substring(start, end).contains('\n')) continue
                builder.setSpan(
                    FuriganaSpan(span.reading),
                    start,
                    end,
                    Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
                )
            }
            // Every line reserves the furigana row, so lines with and without readings align.
            builder.setSpan(
                FuriganaRowSpan(paint.textSize * RUBY_SCALE * 1.1f),
                0,
                builder.length,
                Spanned.SPAN_INCLUSIVE_INCLUSIVE
            )
        }
        setText(builder, BufferType.SPANNABLE)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        gestures.onTouchEvent(event)
        return true
    }

    /** UTF-16 offset of the character under a point in view coordinates, or -1. */
    fun characterAt(x: Float, y: Float): Int {
        val layout = layout ?: return -1
        val content = text ?: return -1
        if (content.isEmpty()) return -1
        val localX = x - totalPaddingLeft + scrollX
        val localY = y - totalPaddingTop + scrollY
        if (localY < 0 || localY > layout.height) return -1
        val line = layout.getLineForVertical(localY.toInt())
        if (localX < layout.getLineLeft(line) || localX > layout.getLineRight(line)) return -1

        // getOffsetForHorizontal gives the nearest gap between characters; step back when the
        // point lies to the left of that gap.
        var offset = layout.getOffsetForHorizontal(line, localX)
        val lineStart = layout.getLineStart(line)
        val lineEnd = layout.getLineVisibleEnd(line)
        if (offset > lineStart && layout.getPrimaryHorizontal(offset) > localX) offset--
        if (offset >= lineEnd) offset = lineEnd - 1
        if (offset < lineStart) return -1
        if (Character.isLowSurrogate(content[offset]) && offset > lineStart) offset--

        // Inside a furigana span the layout only knows the span as a whole: find the base
        // character from its own measurements.
        val spanned = content as? Spanned ?: return offset
        val span = spanned.getSpans(offset, offset + 1, FuriganaSpan::class.java).firstOrNull()
            ?: return offset
        val start = spanned.getSpanStart(span)
        val end = spanned.getSpanEnd(span)
        val spanLeft = layout.getPrimaryHorizontal(start)
        val baseWidth = paint.measureText(content, start, end)
        var cursor = spanLeft + (span.widthFor(paint, content, start, end) - baseWidth) / 2f
        var index = start
        while (index < end) {
            val next = index + Character.charCount(Character.codePointAt(content, index))
            cursor += paint.measureText(content, index, next)
            if (localX < cursor) return index
            index = next
        }
        return end - Character.charCount(Character.codePointBefore(content, end))
    }

    /** Bounds of the line fragment around [offset], in view coordinates. */
    fun boundsOf(offset: Int): Rect {
        val layout = layout ?: return Rect()
        val safe = offset.coerceIn(0, (text?.length ?: 1) - 1)
        val line = layout.getLineForOffset(safe)
        val left = layout.getPrimaryHorizontal(safe).toInt() + totalPaddingLeft - scrollX
        val top = layout.getLineTop(line) + totalPaddingTop - scrollY
        val bottom = layout.getLineBottom(line) + totalPaddingTop - scrollY
        return Rect(left, top, left + paint.textSize.toInt(), bottom)
    }

    /** Draws a reading centred above its base text. */
    private class FuriganaSpan(val reading: String) : ReplacementSpan() {
        /** Highlight colour behind this run, or 0. */
        var background = 0

        fun widthFor(paint: Paint, text: CharSequence, start: Int, end: Int): Float {
            val base = paint.measureText(text, start, end)
            return max(base, rubyWidth(paint))
        }

        private fun rubyWidth(paint: Paint): Float {
            val size = paint.textSize
            paint.textSize = size * RUBY_SCALE
            val width = paint.measureText(reading)
            paint.textSize = size
            return width
        }

        override fun getSize(
            paint: Paint,
            text: CharSequence,
            start: Int,
            end: Int,
            fm: Paint.FontMetricsInt?
        ): Int = ceil(widthFor(paint, text, start, end)).toInt()

        override fun draw(
            canvas: Canvas,
            text: CharSequence,
            start: Int,
            end: Int,
            x: Float,
            top: Int,
            y: Int,
            bottom: Int,
            paint: Paint
        ) {
            val base = paint.measureText(text, start, end)
            val width = widthFor(paint, text, start, end)
            if (background != 0) {
                val color = paint.color
                val style = paint.style
                paint.color = background
                paint.style = Paint.Style.FILL
                canvas.drawRect(x, top.toFloat(), x + width, bottom.toFloat(), paint)
                paint.color = color
                paint.style = style
            }
            canvas.drawText(text, start, end, x + (width - base) / 2f, y.toFloat(), paint)

            val size = paint.textSize
            val ascent = paint.ascent() // negative: distance from the baseline to the glyph tops
            paint.textSize = size * RUBY_SCALE
            val ruby = paint.measureText(reading)
            canvas.drawText(
                reading,
                x + (width - ruby) / 2f,
                y + ascent - paint.descent() * 0.4f,
                paint
            )
            paint.textSize = size
        }
    }

    /** Makes room above each line for the furigana row. */
    private class FuriganaRowSpan(extra: Float) : LineHeightSpan {
        private val extra = ceil(extra).toInt()

        override fun chooseHeight(
            text: CharSequence,
            start: Int,
            end: Int,
            spanstartv: Int,
            lineHeight: Int,
            fm: Paint.FontMetricsInt
        ) {
            // The layout hands over each line's natural metrics afresh, so this adds once.
            fm.ascent -= extra
            fm.top -= extra
        }
    }

    companion object {
        /** How the looked-up word is marked in every reader text view (ReaderSettings.wordMark). */
        @JvmStatic var markStyle: String = ReaderSettings.WORD_MARK_OUTLINE
        const val RUBY_SCALE = 0.5f
    }
}
