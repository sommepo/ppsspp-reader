// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.stats

import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/**
 * Counting rules. Days, sessions and reading time are VN Library's; characters are counted
 * more strictly here (Japanese characters only) and a line shown again within a session is
 * not counted twice.
 *
 * Counts describe text presented during reading, not comprehension.
 */
object ReadingRules {
    /** A reading day starts at 04:01 local time; 04:00:59 still belongs to the previous day. */
    const val DAY_START_MINUTES = 4 * 60 + 1

    /** A break shorter than this continues the previous session. */
    const val SESSION_GAP_MS = 4 * 60 * 60 * 1000L

    /** Active time stops accruing after this long without reading activity. */
    const val DEFAULT_IDLE_TIMEOUT_MS = 5 * 60 * 1000L

    /** A single heartbeat never credits more than this. */
    const val MAX_HEARTBEAT_MS = 5_000L

    /**
     * Characters of narrative text: the Japanese ones only, kana and kanji. Punctuation,
     * symbols, digits, Latin letters and whitespace are not reading and do not count.
     * Speaker names and ruby readings are separate fields and never reach this function.
     */
    fun countCharacters(text: String): Int {
        var count = 0
        var i = 0
        while (i < text.length) {
            val cp = text.codePointAt(i)
            if (isJapanese(cp)) count++
            i += Character.charCount(cp)
        }
        return count
    }

    /**
     * Kana (with the long vowel mark and the iteration marks, which are part of words; half
     * width too) and kanji (with 々, 〆 and 〇). Not the middle dot, brackets or any other sign.
     */
    fun isJapanese(cp: Int): Boolean = when (cp) {
        in 0x3041..0x3096, in 0x309D..0x309F -> true // hiragana
        in 0x30A1..0x30FA, in 0x30FC..0x30FF -> true // katakana, ー
        in 0x31F0..0x31FF -> true // small katakana for Ainu
        in 0xFF66..0xFF9D -> true // half-width katakana
        in 0x3005..0x3007 -> true // 々 〆 〇
        in 0x3400..0x4DBF, in 0x4E00..0x9FFF, in 0xF900..0xFAFF -> true // kanji
        in 0x20000..0x323AF -> true // kanji beyond the basic plane
        else -> false
    }

    /** The reading day (ISO date) that [epochMillis] belongs to in [zone]. */
    fun readingDay(epochMillis: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val local = Instant.ofEpochMilli(epochMillis).atZone(zone).toLocalDateTime()
        val shifted = local.minusMinutes(DAY_START_MINUTES.toLong())
        return LocalDate.of(shifted.year, shifted.month, shifted.dayOfMonth).toString()
    }

    /** Characters per hour, or null below one second of active time. */
    fun charactersPerHour(characters: Long, activeMs: Long): Double? {
        if (activeMs < 1000) return null
        return characters * 3_600_000.0 / activeMs
    }
}
