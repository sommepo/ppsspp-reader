// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.text.NumberFormat
import java.util.Locale
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.SessionSummary
import org.junit.Assert.assertEquals
import org.junit.Test

class SessionSummaryTest {
    private val numbers = NumberFormat.getIntegerInstance(Locale.US)
    private val minute = 60_000L

    private fun summary(characters: Long, activeMs: Long, paused: Boolean = false) =
        SessionSummary.format(ReadingCounters(activeMs = activeMs, characters = characters), paused, numbers)

    @Test
    fun saysCharactersSpeedAndTimeInOneShortLine() {
        assertEquals("1,234字 · 6,170/h · 12m", summary(1234, 12 * minute))
        assertEquals("15,300字 · 9,180/h · 1h40m", summary(15300, 100 * minute))
        assertEquals("48,000字 · 8,000/h · 6h00m", summary(48000, 360 * minute))
    }

    @Test
    fun leavesTheSpeedOutUntilThereIsAMinuteToWorkItOutFrom() {
        assertEquals("12字 · <1m", summary(12, 0))
        assertEquals("40字 · <1m", summary(40, 59_000))
        assertEquals("40字 · 2,400/h · 1m", summary(40, minute))
    }

    @Test
    fun saysWhenNothingIsBeingCounted() {
        assertEquals("paused · 1,234字 · 6,170/h · 12m", summary(1234, 12 * minute, paused = true))
    }

    @Test
    fun theTimeDoesNotTick() {
        assertEquals("<1m", SessionSummary.duration(59_999))
        assertEquals("1m", SessionSummary.duration(60_000))
        assertEquals("59m", SessionSummary.duration(60 * minute - 1))
        assertEquals("1h00m", SessionSummary.duration(60 * minute))
        assertEquals("2h05m", SessionSummary.duration(125 * minute))
    }
}
