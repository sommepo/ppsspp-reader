// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.time.LocalDateTime
import java.time.ZoneId
import org.ppsspp.ppsspp.reader.stats.ReadingRules
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ReadingRulesTest {
    private val tokyo = ZoneId.of("Asia/Tokyo")

    private fun millis(text: String) =
        LocalDateTime.parse(text).atZone(tokyo).toInstant().toEpochMilli()

    @Test
    fun countsJapaneseCharactersOnly() {
        assertEquals(0, ReadingRules.countCharacters(""))
        assertEquals(0, ReadingRules.countCharacters(" \n\t　"))
        // Kana and kanji count; punctuation, the line break and the ideographic space do not.
        assertEquals(10, ReadingRules.countCharacters("…君は旅人だ？\n道を見張れ"))
        assertEquals(13, ReadingRules.countCharacters("…なんだ　そんなとこにいたのか"))
        // Nothing of signs, digits and Latin letters, in either width.
        assertEquals(0, ReadingRules.countCharacters("！？「」（）、。・…―〜♪★ 123 １２３ abc ＡＢＣ %"))
        assertEquals(4, ReadingRules.countCharacters("Lv.5の「剣」を3本"))
        // The long vowel mark and the iteration marks are part of words; so is half-width kana.
        assertEquals(4, ReadingRules.countCharacters("コーヒー"))
        assertEquals(4, ReadingRules.countCharacters("時々、ゝゞ"))
        assertEquals(2, ReadingRules.countCharacters("ｶﾞｷ"))
        // A character outside the BMP is one character, not two.
        assertEquals(3, ReadingRules.countCharacters("𠮷野家"))
    }

    @Test
    fun readingDayStartsAtFourOhOne() {
        assertEquals("2026-09-30", ReadingRules.readingDay(millis("2026-10-01T04:00:59"), tokyo))
        assertEquals("2026-10-01", ReadingRules.readingDay(millis("2026-10-01T04:01:00"), tokyo))
        assertEquals("2026-10-01", ReadingRules.readingDay(millis("2026-10-01T23:59:59"), tokyo))
        assertEquals("2026-10-01", ReadingRules.readingDay(millis("2026-10-02T00:30:00"), tokyo))
        // Month and year boundaries follow the same shift.
        assertEquals("2025-12-31", ReadingRules.readingDay(millis("2026-01-01T03:00:00"), tokyo))
    }

    @Test
    fun speedNeedsASecondOfReading() {
        assertNull(ReadingRules.charactersPerHour(500, 999))
        assertEquals(3600.0, ReadingRules.charactersPerHour(1, 1000)!!, 0.0001)
        assertEquals(12000.0, ReadingRules.charactersPerHour(6000, 1_800_000)!!, 0.0001)
    }
}
