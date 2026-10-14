// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.screen

import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.model.ScreenLayout

/** A line together with where its text is in the game's picture right now. */
data class ScreenText(val presented: PresentedLine, val layout: ScreenLayout)

/**
 * Keeps track of which lines are on screen and where. A line and the layout reported for it
 * arrive separately and in either order (the line passes through the session's worker first),
 * so whichever comes first waits for the other. One thread only.
 */
class ScreenTexts {
    private val lines = HashMap<String, PresentedLine>()
    private val waiting = HashMap<String, ScreenLayout>()
    private val shown = LinkedHashMap<String, ScreenText>()

    /** The lines whose text the game is showing, with where each character is. */
    val onScreen: Collection<ScreenText> get() = shown.values

    /** Where [presented] is on screen, or null when its text is not shown (any more). */
    fun placementOf(presented: PresentedLine): ScreenText? =
        shown[presented.line.window]?.takeIf { it.presented.backlogId == presented.backlogId }

    /**
     * [presented] is now the text of its window. Returns true when [onScreen] changed.
     */
    fun adoptLine(presented: PresentedLine): Boolean {
        val window = presented.line.window
        if (window.isEmpty()) return false
        lines[window] = presented
        // A placement shown so far was for the text this one replaces.
        var changed = shown.remove(window) != null
        val early = waiting[window]
        if (early != null && early.sequence <= presented.line.sequence) {
            waiting.remove(window)
            if (early.sequence == presented.line.sequence) {
                shown[window] = ScreenText(presented, early)
                changed = true
            }
        }
        return changed
    }

    /** A layout arrived from the game. Returns true when [onScreen] changed. */
    fun applyLayout(layout: ScreenLayout): Boolean {
        val window = layout.window
        if (layout.closed) {
            waiting.remove(window)
            return shown.remove(window) != null
        }
        val line = lines[window]
        return when {
            line != null && line.line.sequence == layout.sequence -> {
                shown[window] = ScreenText(line, layout)
                true
            }
            // Its line is still on the way.
            line == null || line.line.sequence < layout.sequence -> {
                waiting[window] = layout
                false
            }
            // For text that has been replaced since.
            else -> false
        }
    }

    /** Forgets everything. Returns true when [onScreen] changed. */
    fun clear(): Boolean {
        lines.clear()
        waiting.clear()
        val changed = shown.isNotEmpty()
        shown.clear()
        return changed
    }
}
