// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.screen

import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.model.ScreenLayout

/**
 * The text of one of the game's text boxes as it sits on a display surface: answers what is
 * under a finger, and where a word is so that it can be marked.
 *
 * @property box The text box as far as touches go: nothing inside it reaches the game.
 */
class PlacedText(
    val window: String,
    val presented: PresentedLine,
    val box: Area,
    val characters: List<Glyph>
) {
    /**
     * One character of the line.
     *
     * @property offset Index into the line's text (UTF-16), where a lookup would start.
     * @property drawn The cell the game drew the character in.
     * @property touch What a finger has to hit: the cell grown to meet its neighbours, the gap
     * above its row (where the game puts furigana) and some room at the ends of a row.
     */
    class Glyph(val offset: Int, val row: Int, val drawn: Area, val touch: Area)

    /** What a point on the surface is, as far as this text is concerned. */
    sealed class Hit {
        /** Not ours: the game gets the touch. */
        object Outside : Hit()

        /** In the text box, but on no character. */
        class Box(val text: PlacedText) : Hit()

        class OnCharacter(val text: PlacedText, val character: Glyph) : Hit()
    }

    /** The rows of text with their margins: what a popup about a word should not cover. */
    val rows: Area = characters.map { it.touch }.reduce(Area::union)

    fun hit(x: Float, y: Float): Hit {
        for (character in characters) {
            if (character.touch.contains(x, y)) return Hit.OnCharacter(this, character)
        }
        return if (box.contains(x, y)) Hit.Box(this) else Hit.Outside
    }

    /** One rectangle per row for the characters at [range] of the line's text. */
    fun marks(range: IntRange): List<Area> {
        val rows = LinkedHashMap<Int, Area>()
        for (character in characters) {
            if (character.offset !in range) continue
            rows[character.row] = rows[character.row]?.union(character.drawn) ?: character.drawn
        }
        return rows.values.toList()
    }

    companion object {
        /** Room for a finger left of a row's first and right of its last character, in cells. */
        private const val END_ROOM = 0.6f

        /** Room above the first row (furigana) and below the last, in row heights. */
        private const val ROOM_ABOVE = 0.6f
        private const val ROOM_BELOW = 0.35f

        /**
         * Places [presented], whose text is laid out as [layout], where [geometry] shows the
         * game's picture. Empty when the picture is not on this surface or nothing is drawn
         * yet.
         */
        fun place(
            presented: PresentedLine,
            layout: ScreenLayout,
            geometry: ScreenGeometry
        ): List<PlacedText> = geometry.areasOf(ScreenLayout.SCREEN_GAME).mapNotNull { area ->
            placeIn(presented, layout, area)
        }

        private class Cell(val offset: Int, val drawn: Area)

        private fun placeIn(presented: PresentedLine, layout: ScreenLayout, area: Area): PlacedText? {
            val text = presented.line.text
            val cells = ArrayList<Cell>()
            var offset = 0
            var index = 0
            while (offset < text.length && index < layout.cells.size) {
                val cell = layout.cells[index]
                if (cell != null && cell.width > 0f && cell.height > 0f) {
                    cells.add(Cell(offset, ScreenGeometry.place(cell, layout, area)))
                }
                offset += Character.charCount(text.codePointAt(offset))
                index++
            }
            if (cells.isEmpty()) return null

            // Rows: characters follow each other in a row as long as they stay level.
            val rows = ArrayList<MutableList<Cell>>()
            for (cell in cells) {
                val row = rows.lastOrNull()
                val last = row?.last()?.drawn
                val level = last != null &&
                    kotlin.math.abs((cell.drawn.top + cell.drawn.bottom) - (last.top + last.bottom)) <
                    minOf(cell.drawn.height, last.height)
                if (row != null && level) row.add(cell) else rows.add(mutableListOf(cell))
            }

            val characters = ArrayList<Glyph>(cells.size)
            var previousBottom = 0f
            for ((rowIndex, row) in rows.withIndex()) {
                val rowTop = row.minOf { it.drawn.top }
                val rowBottom = row.maxOf { it.drawn.bottom }
                val rowHeight = rowBottom - rowTop
                // The gap above a row belongs to it: that is where its furigana is.
                val top = if (rowIndex == 0) rowTop - rowHeight * ROOM_ABOVE else minOf(previousBottom, rowTop)
                val bottom = if (rowIndex == rows.lastIndex) rowBottom + rowHeight * ROOM_BELOW else rowBottom
                for ((i, cell) in row.withIndex()) {
                    val drawn = cell.drawn
                    val left = if (i == 0) {
                        drawn.left - drawn.width * END_ROOM
                    } else {
                        (row[i - 1].drawn.right + drawn.left) / 2f
                    }
                    val right = if (i == row.lastIndex) {
                        drawn.right + drawn.width * END_ROOM
                    } else {
                        (drawn.right + row[i + 1].drawn.left) / 2f
                    }
                    // Never beyond the game's picture: the borders beside it are not the
                    // game's text box.
                    val touch = Area(left, top, right, bottom).clampedTo(area)
                    characters.add(Glyph(cell.offset, rowIndex, drawn, touch))
                }
                previousBottom = rowBottom
            }

            var box = layout.box?.let { ScreenGeometry.place(it, layout, area) }
            for (character in characters) box = box?.union(character.touch) ?: character.touch
            return PlacedText(layout.window, presented, box!!.clampedTo(area), characters)
        }
    }
}
