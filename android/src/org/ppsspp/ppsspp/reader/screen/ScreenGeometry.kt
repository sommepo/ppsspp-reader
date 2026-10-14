// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.screen

import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.ui.CrtSettings

/** A rectangle in pixels of a display surface. */
data class Area(val left: Float, val top: Float, val right: Float, val bottom: Float) {
    val width: Float get() = right - left
    val height: Float get() = bottom - top
    val isEmpty: Boolean get() = right <= left || bottom <= top

    fun contains(x: Float, y: Float): Boolean = x >= left && x < right && y >= top && y < bottom

    fun union(other: Area): Area = Area(
        minOf(left, other.left),
        minOf(top, other.top),
        maxOf(right, other.right),
        maxOf(bottom, other.bottom)
    )

    /** The part of this rectangle inside [bounds] (possibly empty). */
    fun clampedTo(bounds: Area): Area = Area(
        left.coerceIn(bounds.left, bounds.right),
        top.coerceIn(bounds.top, bounds.bottom),
        right.coerceIn(bounds.left, bounds.right),
        bottom.coerceIn(bounds.top, bounds.bottom)
    )
}

/**
 * What one display surface shows: the game's picture, somewhere on the surface of the display
 * the game is on, or the trackpad, which is all of the second display's surface.
 */
data class ScreenGeometry(
    val width: Int,
    val height: Int,
    val game: List<Area>,
    val pad: List<Area>
) {
    fun areasOf(screen: Int): List<Area> = if (screen == ScreenLayout.SCREEN_PAD) pad else game

    /** The same arrangement seen from a view whose origin is ([dx], [dy]) away. */
    fun movedBy(dx: Float, dy: Float): ScreenGeometry {
        if (dx == 0f && dy == 0f) return this
        fun moved(areas: List<Area>) =
            areas.map { Area(it.left + dx, it.top + dy, it.right + dx, it.bottom + dy) }
        return ScreenGeometry(width, height, moved(game), moved(pad))
    }

    companion object {
        /** A rectangle given in the units of [layout], as drawn where the picture is [area]. */
        fun place(cell: ScreenLayout.Cell, layout: ScreenLayout, area: Area): Area {
            val scaleX = area.width / layout.spaceWidth
            val scaleY = area.height / layout.spaceHeight
            val placed = Area(
                area.left + cell.x * scaleX,
                area.top + cell.y * scaleY,
                area.left + cell.right * scaleX,
                area.top + cell.bottom * scaleY
            )
            // The CRT picture's glass bends the picture: the cell goes with its middle.
            if (area.width <= 0f || area.height <= 0f) return placed
            val middleX = (placed.left + placed.right) / 2f
            val middleY = (placed.top + placed.bottom) / 2f
            val point = floatArrayOf((middleX - area.left) / area.width, (middleY - area.top) / area.height)
            CrtSettings.toScreen(point)
            val dx = area.left + point[0] * area.width - middleX
            val dy = area.top + point[1] * area.height - middleY
            return if (dx == 0f && dy == 0f) placed else Area(placed.left + dx, placed.top + dy, placed.right + dx, placed.bottom + dy)
        }

        /**
         * Reads what ReaderNative.pictureRect returns: the surface's width and height, then
         * the picture's left, top, right and bottom. Null when there is nothing to map: no
         * surface, or no picture on it yet.
         */
        fun fromPicture(values: IntArray?): ScreenGeometry? {
            if (values == null || values.size < 6) return null
            val picture = Area(
                values[2].toFloat(),
                values[3].toFloat(),
                values[4].toFloat(),
                values[5].toFloat()
            )
            if (picture.isEmpty) return null
            return ScreenGeometry(values[0], values[1], listOf(picture), emptyList())
        }

        /** The second display: all of it is the trackpad. */
        fun padOnly(width: Int, height: Int): ScreenGeometry? {
            if (width <= 0 || height <= 0) return null
            return ScreenGeometry(width, height, emptyList(), listOf(Area(0f, 0f, width.toFloat(), height.toFloat())))
        }
    }
}
