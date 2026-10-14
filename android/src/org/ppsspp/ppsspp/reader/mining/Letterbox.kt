// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import android.graphics.Bitmap

/** Removes the black bars the screen layout leaves around the game picture. */
object Letterbox {
    /** Left, top, right (exclusive), bottom (exclusive). */
    data class Bounds(val left: Int, val top: Int, val right: Int, val bottom: Int) {
        val width get() = right - left
        val height get() = bottom - top
    }

    /**
     * The picture without its pure-black border rows and columns. A picture that is mostly black
     * (a fade, a dark scene) is left whole rather than cropped to a sliver.
     */
    fun bounds(pixels: IntArray, width: Int, height: Int): Bounds {
        val full = Bounds(0, 0, width, height)
        if (width <= 0 || height <= 0 || pixels.size < width * height) return full

        fun rowBlack(y: Int): Boolean {
            val base = y * width
            for (x in 0 until width) if (pixels[base + x] and 0x00FFFFFF != 0) return false
            return true
        }

        fun columnBlack(x: Int, top: Int, bottom: Int): Boolean {
            for (y in top until bottom) if (pixels[y * width + x] and 0x00FFFFFF != 0) return false
            return true
        }

        var top = 0
        while (top < height && rowBlack(top)) top++
        if (top == height) return full
        var bottom = height
        while (bottom > top && rowBlack(bottom - 1)) bottom--
        var left = 0
        while (left < width && columnBlack(left, top, bottom)) left++
        var right = width
        while (right > left && columnBlack(right - 1, top, bottom)) right--

        val trimmed = Bounds(left, top, right, bottom)
        val keepsEnough =
            trimmed.width.toLong() * trimmed.height * 10 >= width.toLong() * height * 3
        return if (keepsEnough) trimmed else full
    }

    /** [bitmap] cropped to [bounds]; the same object when there is nothing to remove. */
    fun trim(bitmap: Bitmap): Bitmap {
        val width = bitmap.width
        val height = bitmap.height
        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
        val b = bounds(pixels, width, height)
        if (b.width == width && b.height == height) return bitmap
        return Bitmap.createBitmap(bitmap, b.left, b.top, b.width, b.height)
    }
}
