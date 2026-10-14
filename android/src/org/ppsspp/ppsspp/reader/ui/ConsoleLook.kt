// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.drawable.Drawable
import android.view.View
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * The console's look for the word popup, the question about a card's audio and the pointer:
 * the same as the desktop app's (its `reader_overlay.cpp`, "The console's look").
 * Here: the PSP's cross-media bar: a blue band with the wave through it, white type, the
 * headword aglow; the pointer is one of the lights that drift along the wave.
 */
@Suppress("unused")
object ConsoleLook {
    private fun argb(a: Int, r: Int, g: Int, b: Int) = (a shl 24) or (r shl 16) or (g shl 8) or b

    private abstract class Plain : Drawable() {
        protected val paint = Paint(Paint.ANTI_ALIAS_FLAG)

        override fun setAlpha(alpha: Int) = Unit

        override fun setColorFilter(filter: ColorFilter?) = Unit

        @Deprecated("Deprecated in Java")
        override fun getOpacity() = PixelFormat.TRANSLUCENT
    }

    /** The popup's ground and its edge. */
    fun ground(density: Float): Drawable = object : Plain() {
        override fun draw(canvas: Canvas) {
            val area = RectF(bounds)
            val one = max(1f, density)
            paint.style = Paint.Style.FILL
            paint.shader = LinearGradient(
                area.left, area.top, area.right, area.bottom,
                intArrayOf(argb(255, 40, 110, 200), argb(255, 20, 50, 130), argb(255, 60, 140, 220)),
                floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP
            )
            canvas.drawRect(area, paint)
            paint.shader = null
            // the wave that runs through the menu
            canvas.save()
            canvas.clipRect(area)
            val wave = RectF(
                area.left - area.width() * 0.1f, area.top + area.height() * 0.40f,
                area.left + area.width() * 1.1f, area.top + area.height() * 1.70f
            )
            paint.color = argb(26, 160, 210, 255)
            canvas.drawOval(wave, paint)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 1.5f * density
            paint.color = argb(72, 255, 255, 255)
            canvas.drawOval(wave, paint)
            canvas.restore()
            paint.style = Paint.Style.FILL
            paint.color = argb(153, 255, 255, 255)
            canvas.drawRect(area.left, area.top, area.right, area.top + one, paint)
            paint.color = argb(77, 255, 255, 255)
            canvas.drawRect(area.left, area.bottom - one, area.right, area.bottom, paint)
        }
    }

    /** The rule between two entries. */
    fun rule(density: Float): Drawable = object : Plain() {
        override fun getIntrinsicHeight() = 1

        override fun draw(canvas: Canvas) {
            val area = RectF(bounds)
            val one = max(1f, density)
            paint.style = Paint.Style.FILL
            paint.shader = LinearGradient(
                area.left, 0f, area.right, 0f,
                intArrayOf(argb(0, 255, 255, 255), argb(180, 255, 255, 255), argb(0, 255, 255, 255)),
                floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP
            )
            canvas.drawRect(area, paint)
            paint.shader = null
        }
    }

    /**
     * What lies behind an entry, [head] being its headword's line and [first] true for the
     * entry at the top; null where the look has nothing there.
     */
    fun entry(density: Float, first: Boolean, head: View): Drawable? = object : Plain() {
        override fun draw(canvas: Canvas) {
            // the headword, lit as the menu's chosen entry is
            val top = (head.parent as? View)?.let { it.top + head.top }?.toFloat() ?: head.top.toFloat()
            val height = head.height.toFloat()
            if (height <= 0f) return
            val left = bounds.left + head.left.toFloat()
            paint.style = Paint.Style.FILL
            paint.shader = RadialGradient(
                left + height * 1.2f, top + height / 2, height * 2.2f, argb(70, 255, 255, 255), argb(0, 255, 255, 255), Shader.TileMode.CLAMP
            )
            canvas.drawRect(
                max(bounds.left.toFloat(), left - 8 * density), max(bounds.top.toFloat(), top - 6 * density),
                bounds.right.toFloat(), min(bounds.bottom.toFloat(), top + height + 6 * density), paint
            )
            paint.shader = null
        }
    }

    /** The pointer, [size] across, pointing at its own centre. */
    fun drawPointer(canvas: Canvas, paint: Paint, x: Float, y: Float, size: Float) {
        val style = paint.style
        val width = paint.strokeWidth
        val cap = paint.strokeCap
        val join = paint.strokeJoin
        // one of the lights that drift along the wave: a four-point sparkle in a soft glow
        paint.style = Paint.Style.FILL
        paint.shader = RadialGradient(
            x, y, size / 2,
            intArrayOf(argb(255, 255, 255, 255), argb(220, 207, 230, 255), argb(0, 111, 180, 255)),
            floatArrayOf(0f, 0.35f, 1f), Shader.TileMode.CLAMP
        )
        canvas.drawCircle(x, y, size / 2, paint)
        paint.shader = null
        val arm = size * 0.38f
        val waist = size * 0.06f
        val star = Path().apply {
            moveTo(x, y - arm)
            lineTo(x + waist, y - waist)
            lineTo(x + arm, y)
            lineTo(x + waist, y + waist)
            lineTo(x, y + arm)
            lineTo(x - waist, y + waist)
            lineTo(x - arm, y)
            lineTo(x - waist, y - waist)
            close()
        }
        paint.color = argb(255, 255, 255, 255)
        canvas.drawPath(star, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = size * 0.02f
        paint.color = argb(160, 20, 50, 130)
        canvas.drawPath(star, paint)
        paint.shader = null
        paint.style = style
        paint.strokeWidth = width
        paint.strokeCap = cap
        paint.strokeJoin = join
    }
}
