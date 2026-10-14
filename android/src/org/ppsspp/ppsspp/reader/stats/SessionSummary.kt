// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.stats

import java.text.NumberFormat

/** A reading session in a few characters, to sit in a corner of the game: "1,234字 · 5,430/h · 12m". */
object SessionSummary {
    /** A speed worked out from less reading than this jumps about too much to show. */
    const val SPEED_AFTER_MS = 60_000L

    fun format(
        counters: ReadingCounters,
        paused: Boolean = false,
        numbers: NumberFormat = NumberFormat.getIntegerInstance()
    ): String = buildString {
        if (paused) append("paused · ")
        append(numbers.format(counters.characters)).append('字')
        val speed = counters.charactersPerHour
        if (speed != null && counters.activeMs >= SPEED_AFTER_MS) {
            // To the nearest ten: the last digit would change with every line.
            append(" · ").append(numbers.format(Math.round(speed / 10.0) * 10)).append("/h")
        }
        append(" · ").append(duration(counters.activeMs))
    }

    /** "<1m", "12m" or "1h05m": minutes, so that it does not tick. */
    fun duration(activeMs: Long): String {
        val minutes = activeMs / 60_000
        return when {
            minutes < 1 -> "<1m"
            minutes < 60 -> "${minutes}m"
            else -> String.format("%dh%02dm", minutes / 60, minutes % 60)
        }
    }
}
