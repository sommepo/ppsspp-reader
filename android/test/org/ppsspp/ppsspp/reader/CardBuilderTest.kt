// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.anki.CardBuilder
import org.ppsspp.ppsspp.reader.anki.CardContext
import org.ppsspp.ppsspp.reader.dict.Frequency
import org.ppsspp.ppsspp.reader.dict.LookupResult
import org.ppsspp.ppsspp.reader.dict.PitchAccent
import org.ppsspp.ppsspp.reader.dict.TermEntry
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CardBuilderTest {
    private val line = TextLine(
        sequence = 1, frame = 1, audioPosition = 100, channel = "dialogue", speaker = "タロウ",
        text = "ここの道を\n見張れ <now>", ruby = listOf(RubySpan(3, 1, "みち"), RubySpan(6, 2, "みは")),
        sourceId = "s", meta = emptyMap(), timestamp = 1
    )

    private fun entry(dictionary: String, glossary: String, tags: String = "v5") = TermEntry(
        dictionaryId = 1, dictionary = dictionary, priority = 0, expression = "見張る",
        reading = "みはる", definitionTags = tags, rules = "v5", score = 1, glossaryJson = glossary,
        sequence = 1, termTags = ""
    )

    private val word = LookupResult(
        expression = "見張る", reading = "みはる", matchedText = "見張れ", reasons = listOf("imperative"),
        entries = listOf(
            entry("JMdict", """["to keep watch","to stand guard"]"""),
            entry("大辞林", """["注意して番をする。"]""", tags = "")
        ),
        frequencies = listOf(Frequency("JPDB", "4200", 4200.0, true), Frequency("BCCWJ", "9000", 9000.0, true)),
        pitches = listOf(PitchAccent("NHK", "みはる", listOf(2, 0)))
    )

    private val context = CardContext(
        line = line, gameTitle = "真・女神転生IV", word = word, wordRange = 6..8,
        screenshotName = "shot 1.jpg", audioName = "clip.m4a"
    )

    private fun fill(template: String, c: CardContext = context) =
        CardBuilder.build(mapOf("F" to template), c).getValue("F")

    @Test
    fun wordMarkers() {
        assertEquals("見張る", fill("{expression}"))
        assertEquals("みはる", fill("{reading}"))
        assertEquals("見張[みは]る", fill("{furigana}"))
        assertEquals("<ruby>見張<rt>みは</rt></ruby>る", fill("{furigana-html}"))
        assertEquals("見張れ", fill("{matched-text}"))
        assertEquals("imperative", fill("{inflection}"))
        assertEquals("v5", fill("{part-of-speech}"))
        assertEquals("JPDB: 4200<br>BCCWJ: 9000", fill("{frequencies}"))
        assertEquals("5727", fill("{frequency-rank}")) // harmonic mean of 4200 and 9000
        assertEquals("2, 0", fill("{pitch-positions}"))
    }

    @Test
    fun sentenceMarkersEscapeTheGameText() {
        assertEquals("ここの道を<br><b>見張れ</b> &lt;now&gt;", fill("{sentence}"))
        assertEquals("ここの道を<br>見張れ &lt;now&gt;", fill("{sentence-plain}"))
        // Anki's furigana filter ends a base at a space or a tag, so none is needed after <br>.
        assertEquals("ここの 道[みち]を<br>見張[みは]れ &lt;now&gt;", fill("{sentence-furigana}"))
        assertEquals(
            "ここの<ruby>道<rt>みち</rt></ruby>を<br><ruby>見張<rt>みは</rt></ruby>れ &lt;now&gt;",
            fill("{sentence-furigana-html}")
        )
        assertEquals("タロウ", fill("{speaker}"))
        assertEquals("真・女神転生IV", fill("{source}"))
        // The game's page, as {url} is the page's address in Yomitan; nothing without a game.
        assertEquals("", fill("{url}"))
        val addressed = context.copy(gameUrl = "https://ec.nintendo.com/apps/uljm05000/JP")
        assertEquals("https://ec.nintendo.com/apps/uljm05000/JP", fill("{url}", addressed))
        assertEquals(
            "<a href=\"https://ec.nintendo.com/apps/uljm05000/JP\">真・女神転生IV</a>",
            fill("<a href=\"{url}\">{source}</a>", addressed)
        )
    }

    @Test
    fun mediaMarkers() {
        assertEquals("<img src=\"shot 1.jpg\">", fill("{screenshot}"))
        assertEquals("[sound:clip.m4a]", fill("{audio}"))
        val bare = context.copy(screenshotName = null, audioName = null)
        assertEquals("", fill("{screenshot}", bare))
        assertEquals("", fill("{audio}", bare))
    }

    @Test
    fun glossaryListsEveryDictionaryInYomitanStructure() {
        assertEquals(
            "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>" +
                "<li data-dictionary=\"JMdict\"><i>(v5, JMdict)</i> " +
                "<ul><li>to keep watch</li><li>to stand guard</li></ul></li>" +
                "<li data-dictionary=\"大辞林\"><i>(大辞林)</i> 注意して番をする。</li>" +
                "</ol></div>",
            fill("{glossary}")
        )
        val first = fill("{glossary-first}")
        assertTrue(first.contains("JMdict"))
        assertFalse(first.contains("大辞林"))
        assertEquals("to keep watch; to stand guard<br>注意して番をする。", fill("{glossary-plain}"))
    }

    @Test
    fun templatesMixMarkersAndTextAndKeepUnknownMarkersVisible() {
        assertEquals("見張る【みはる】", fill("{expression}【{reading}】"))
        assertEquals("{expresion}", fill("{expresion}"))
        assertEquals("plain text", fill("plain text"))
    }

    @Test
    fun onASentenceCardTheLineStandsInForTheWord() {
        val sentenceOnly = CardContext(line = line, gameTitle = "G")
        // A note type made for words has the word as its first field, and Anki takes no note
        // whose first field is empty.
        assertEquals("ここの道を<br>見張れ &lt;now&gt;", fill("{expression}", sentenceOnly))
        assertEquals(fill("{sentence-furigana}", sentenceOnly), fill("{furigana}", sentenceOnly))
        assertEquals(fill("{sentence-furigana-html}", sentenceOnly), fill("{furigana-html}", sentenceOnly))
        assertEquals("", fill("{reading}", sentenceOnly))
        assertEquals("", fill("{glossary}", sentenceOnly))
        assertEquals("ここの道を<br>見張れ &lt;now&gt;", fill("{sentence}", sentenceOnly))
    }

    @Test
    fun detectsWhichMediaAreNeeded() {
        val templates = listOf("{expression}", "{sentence}<br>{screenshot}", "")
        assertTrue(CardBuilder.needsScreenshot(templates))
        assertFalse(CardBuilder.needsAudio(templates))
        assertEquals(setOf("expression", "sentence", "screenshot"), CardBuilder.markersIn(templates))
    }

    @Test
    fun suggestsTemplatesForCommonNoteTypes() {
        val fields = listOf(
            "Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "MainDefinition",
            "Sentence", "SentenceFurigana", "SentenceAudio", "Picture", "FreqSort", "PitchPosition",
            "MiscInfo", "Hint"
        )
        val suggestion = CardBuilder.suggestMapping(fields)
        assertEquals(fields, suggestion.keys.toList())
        assertEquals("{expression}", suggestion["Expression"])
        assertEquals("{furigana}", suggestion["ExpressionFurigana"])
        assertEquals("{reading}", suggestion["ExpressionReading"])
        assertEquals("{word-audio}", suggestion["ExpressionAudio"]) // the word as somebody says it
        assertEquals("{glossary}", suggestion["MainDefinition"])
        assertEquals("{sentence}", suggestion["Sentence"])
        assertEquals("{sentence-furigana}", suggestion["SentenceFurigana"])
        assertEquals("{audio}", suggestion["SentenceAudio"])
        assertEquals("{screenshot}", suggestion["Picture"])
        assertEquals("{frequency-rank}", suggestion["FreqSort"])
        assertEquals("{pitch-positions}", suggestion["PitchPosition"])
        assertEquals("{source}", suggestion["MiscInfo"])
        assertEquals("", suggestion["Hint"])
    }

    @Test
    fun mappingRoundTripsThroughStorage() {
        val mapping = linkedMapOf("Expression" to "{expression}", "Notes" to "from \"{source}\"")
        assertEquals(mapping, CardBuilder.parseMapping(CardBuilder.mappingToJson(mapping)))
        assertTrue(CardBuilder.parseMapping("").isEmpty())
        assertTrue(CardBuilder.parseMapping("not json").isEmpty())
    }

    @Test
    fun everyAdvertisedMarkerIsImplemented() {
        for ((marker, _) in CardBuilder.MARKERS) {
            assertFalse("{$marker} is listed but not handled", fill("{$marker}") == "{$marker}")
        }
    }
}
