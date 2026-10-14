// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TextLineTest {
    // The shape the core emits (see Reader::TextEvent::ToJson).
    private val json = """{"seq":7,"frame":685,"audio":374720,"channel":"dialogue","speaker":"声",
        "text":"…きっと来るよ\nそれでボクも形が保てる…","ruby":[[4,1,"く"],[14,1,"かたち"],[16,1,"たも"]],
        "id":"0123456789abcdef","meta":{"page":1,"pages":1,"voice":2004}}"""

    @Test
    fun parsesEngineEvent() {
        val line = TextLine.fromJson(json, 1234L)!!
        assertEquals(7, line.sequence)
        assertEquals(685, line.frame)
        assertEquals(374720, line.audioPosition)
        assertEquals("dialogue", line.channel)
        assertEquals("声", line.speaker)
        assertEquals("…きっと来るよ\nそれでボクも形が保てる…", line.text)
        assertEquals(
            listOf(RubySpan(4, 1, "く"), RubySpan(14, 1, "かたち"), RubySpan(16, 1, "たも")),
            line.ruby
        )
        assertEquals("0123456789abcdef", line.sourceId)
        assertEquals("2004", line.meta["voice"])
        assertEquals(1234L, line.timestamp)
    }

    @Test
    fun rejectsEmptyAndBrokenEvents() {
        assertNull(TextLine.fromJson("not json", 0))
        assertNull(TextLine.fromJson("""{"text":""}""", 0))
    }

    @Test
    fun dropsRubyOutsideTheText() {
        val line = TextLine.fromJson("""{"text":"名前","ruby":[[0,2,"なまえ"],[1,5,"x"],[-1,1,"y"]]}""", 0)!!
        assertEquals(listOf(RubySpan(0, 2, "なまえ")), line.ruby)
    }

    @Test
    fun rubySegmentsFollowCodePoints() {
        val line = TextLine.fromJson(json, 0)!!
        val segments = ArrayList<Pair<String, String?>>()
        line.forEachSegment { base, reading -> segments.add(base to reading) }
        assertEquals(
            listOf(
                "…きっと" to null, "来" to "く", "るよ\nそれでボクも" to null, "形" to "かたち",
                "が" to null, "保" to "たも", "てる…" to null
            ),
            segments
        )
        // Offsets are code points: a surrogate pair before the span must not shift it.
        val astral = TextLine.fromJson("""{"text":"𠮷野家へ","ruby":[[1,2,"のや"]]}""", 0)!!
        val parts = ArrayList<Pair<String, String?>>()
        astral.forEachSegment { base, reading -> parts.add(base to reading) }
        assertEquals(listOf("𠮷" to null, "野家" to "のや", "へ" to null), parts)
    }

    @Test
    fun exportsFuriganaForAnki() {
        val line = TextLine.fromJson(json, 0)!!
        assertEquals("…きっと 来[く]るよ\nそれでボクも 形[かたち]が 保[たも]てる…", line.toAnkiFurigana())
        assertTrue(line.toRubyHtml().contains("<ruby>形<rt>かたち</rt></ruby>"))
        assertTrue(line.toRubyHtml().contains("るよ<br>それで"))
    }

    @Test
    fun roundTripsThroughJson() {
        val line = TextLine.fromJson(json, 99L)!!
        val again = TextLine.fromJson(line.toJson().toString(), 5L)!!
        assertEquals(line, again)
    }
}
