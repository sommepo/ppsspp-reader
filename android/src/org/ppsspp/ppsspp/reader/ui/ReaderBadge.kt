// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.util.TypedValue
import android.view.View
import androidx.appcompat.content.res.AppCompatResources
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.ReaderCatalog

/**
 * The mark of a game whose text the reader can read: a small boxed book in the top right corner
 * of the game's picture in the game list. It is drawn over the picture as the view's foreground,
 * so the list's layouts stay as they are.
 */
object ReaderBadge {
    /**
     * Shows the badge on [picture], the cover of the game [titleId] (sixteen hexadecimal
     * digits), when that game has a text profile, and takes it off otherwise: list rows are
     * reused for other games.
     */
    fun mark(picture: View, titleId: String) {
        val readable = ReaderCatalog.covers(picture.context.applicationContext, titleId)
        if (readable) {
            if (picture.foreground !is Badge) picture.foreground = Badge(picture.context)
        } else if (picture.foreground is Badge) {
            picture.foreground = null
        }
    }

    /** True when [picture] shows the badge. */
    fun isOn(picture: View): Boolean = picture.foreground is Badge

    private class Badge(context: Context) : Drawable() {
        private val density = context.resources.displayMetrics
        private val size = dp(26f)
        private val margin = dp(6f)
        private val box = RectF()
        private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = ReaderStyle.POPUP_BACKGROUND }
        private val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = dp(1.5f)
            color = ReaderStyle.ACCENT
        }
        private val book = AppCompatResources.getDrawable(context, R.drawable.ic_reader_open_book)?.mutate()
            ?.apply { setTint(ReaderStyle.ACCENT) }

        private fun dp(value: Float) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, density)

        override fun draw(canvas: Canvas) {
            val area = bounds
            // A cover smaller than a few badges across gets a badge to match.
            val side = minOf(size, area.width() / 3f, area.height() / 3f)
            if (side <= 0f) return
            val gap = margin * side / size
            box.set(area.right - gap - side, area.top + gap, area.right - gap, area.top + gap + side)
            val corner = side / 5f
            canvas.drawRoundRect(box, corner, corner, fill)
            canvas.drawRoundRect(box, corner, corner, edge)
            val inset = side * 0.2f
            book?.setBounds(
                (box.left + inset).toInt(),
                (box.top + inset).toInt(),
                (box.right - inset).toInt(),
                (box.bottom - inset).toInt()
            )
            book?.draw(canvas)
        }

        override fun setAlpha(alpha: Int) = Unit

        override fun setColorFilter(colorFilter: ColorFilter?) = Unit

        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
    }
}
