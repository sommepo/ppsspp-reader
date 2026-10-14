// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.anki

import org.ppsspp.ppsspp.reader.dict.DictionaryLookup
import org.ppsspp.ppsspp.reader.dict.GlossaryRenderer
import org.ppsspp.ppsspp.reader.dict.JapaneseText
import org.ppsspp.ppsspp.reader.dict.LookupResult
import org.ppsspp.ppsspp.reader.model.TextLine
import org.json.JSONObject

/** Everything a card can be made from. Only [line] is always present. */
data class CardContext(
    val line: TextLine,
    val gameTitle: String,
    /** The looked-up word, or null for a card of the sentence alone. */
    val word: LookupResult? = null,
    /** UTF-16 range of the word within the line's text. */
    val wordRange: IntRange? = null,
    /** Stored media file names, once the files are in Anki. */
    val screenshotName: String? = null,
    val audioName: String? = null,
    /** The word as somebody says it ([org.ppsspp.ppsspp.reader.mining.WordAudio]), once it is in Anki. */
    val wordAudioName: String? = null,
    /** The game's page ([SourceAddress.of]); empty when the game is not known. */
    val gameUrl: String = ""
)

/**
 * Fills note fields from templates such as "{expression}" or "{sentence}<br>{screenshot}".
 * A template may mix markers and ordinary text; an unknown marker is left as written so a typo
 * is visible on the card rather than silently empty.
 */
object CardBuilder {
    /** Markers offered in the field mapping screen, with what they produce. */
    val MARKERS: List<Pair<String, String>> = listOf(
        "expression" to "Word in dictionary form (the line, on a sentence card)",
        "reading" to "Reading in kana",
        "furigana" to "Word with reading: 食[た]べる",
        "furigana-html" to "Word with reading as ruby HTML",
        "glossary" to "Definitions from every dictionary",
        "glossary-first" to "Definitions from the first dictionary",
        "glossary-plain" to "Definitions as plain text",
        "sentence" to "The line, with the word in bold",
        "sentence-plain" to "The line as plain text",
        "sentence-furigana" to "The line with the game's furigana: 君[きみ]",
        "sentence-furigana-html" to "The line with the game's furigana as ruby HTML",
        "screenshot" to "Picture of the game screen",
        "audio" to "Game audio of the line",
        "word-audio" to "The word spoken (JapanesePod101)",
        "speaker" to "Who is speaking",
        "source" to "Game title",
        "url" to "The game's page, as a web address",
        "frequencies" to "Frequency list entries",
        "frequency-rank" to "One frequency rank for sorting",
        "pitch-positions" to "Pitch accent positions",
        "part-of-speech" to "Word class tags",
        "inflection" to "How the word was inflected in the line",
        "matched-text" to "The word as written in the line"
    )

    // Android's regex engine rejects a bare closing brace that the desktop JVM lets through.
    private val MARKER = Regex("\\{([a-z\\-]+)\\}")

    /** Markers used anywhere in [templates]. */
    fun markersIn(templates: Collection<String>): Set<String> =
        templates.flatMap { t -> MARKER.findAll(t).map { it.groupValues[1] } }.toSet()

    fun needsScreenshot(templates: Collection<String>) = "screenshot" in markersIn(templates)

    fun needsAudio(templates: Collection<String>) = "audio" in markersIn(templates)

    fun needsWordAudio(templates: Collection<String>) = "word-audio" in markersIn(templates)

    /** Fields that are for the word's own sound, by the names popular note types give them. */
    private val WORD_AUDIO_FIELDS =
        setOf("wordaudio", "expressionaudio", "vocabaudio", "termaudio", "audio", "vocabularyaudio")

    private fun plainName(field: String) = field.lowercase().replace(Regex("[\\s_\\-]"), "")

    /**
     * [mapping] with {word-audio} in the fields that are for the word's sound and have nothing
     * in them: a mapping made before that marker existed left them empty.
     */
    fun offerWordAudio(mapping: Map<String, String>): Map<String, String> =
        mapping.mapValues { (field, template) ->
            if (template.isBlank() && plainName(field) in WORD_AUDIO_FIELDS) "{word-audio}" else template
        }

    /** Field values for [templates] (field name to template). */
    fun build(templates: Map<String, String>, context: CardContext): Map<String, String> =
        templates.mapValues { (_, template) ->
            MARKER.replace(template) { match -> value(match.groupValues[1], context) ?: match.value }
        }

    private fun value(marker: String, c: CardContext): String? {
        val word = c.word
        return when (marker) {
            // A card of the sentence alone has no word. The line stands in for it, so that a
            // note type made for words (whose first field is the word) still gets a note.
            "expression" -> word?.expression ?: JapaneseText.escapeHtml(c.line.text).replace("\n", "<br>")

            "reading" -> word?.let { it.reading.ifEmpty { it.expression } } ?: ""

            "furigana" -> word?.let { JapaneseText.ankiFurigana(it.expression, it.reading) }
                ?: JapaneseText.escapeHtml(c.line.toAnkiFurigana()).replace("\n", "<br>")

            "furigana-html" -> word?.let { JapaneseText.rubyHtml(it.expression, it.reading) }
                ?: c.line.toRubyHtml()

            "glossary" -> word?.let { glossary(it, firstOnly = false) } ?: ""

            "glossary-first" -> word?.let { glossary(it, firstOnly = true) } ?: ""

            "glossary-plain" -> word?.entries?.joinToString("\n") { entry ->
                GlossaryRenderer.toPlainText(entry.glossaryJson)
            }?.let(JapaneseText::escapeHtml)?.replace("\n", "<br>") ?: ""

            "sentence" -> sentenceHtml(c.line.text, c.wordRange)

            "sentence-plain" -> JapaneseText.escapeHtml(c.line.text).replace("\n", "<br>")

            "sentence-furigana" ->
                JapaneseText.escapeHtml(c.line.toAnkiFurigana()).replace("\n", "<br>")

            "sentence-furigana-html" -> c.line.toRubyHtml()

            "screenshot" -> c.screenshotName?.let { "<img src=\"${JapaneseText.escapeHtml(it)}\">" } ?: ""

            "audio" -> c.audioName?.let { "[sound:$it]" } ?: ""

            "word-audio" -> c.wordAudioName?.let { "[sound:$it]" } ?: ""

            "speaker" -> JapaneseText.escapeHtml(c.line.speaker)

            "source" -> JapaneseText.escapeHtml(c.gameTitle)

            "url" -> JapaneseText.escapeHtml(c.gameUrl)

            "frequencies" -> word?.frequencies?.joinToString("<br>") {
                JapaneseText.escapeHtml("${it.dictionary}: ${it.display}")
            } ?: ""

            "frequency-rank" ->
                word?.let { DictionaryLookup.harmonicRank(it.frequencies)?.toString() } ?: ""

            "pitch-positions" -> word?.pitches?.flatMap { it.positions }?.distinct()
                ?.joinToString(", ") ?: ""

            "part-of-speech" -> word?.entries?.flatMap { it.definitionTags.split(' ') }
                ?.filter { it.isNotBlank() }?.distinct()?.joinToString(", ")
                ?.let(JapaneseText::escapeHtml) ?: ""

            "inflection" -> word?.reasons?.joinToString(" « ")?.let(JapaneseText::escapeHtml) ?: ""

            "matched-text" -> word?.matchedText?.let(JapaneseText::escapeHtml) ?: ""

            else -> null
        }
    }

    /** The line as HTML with [range] in bold. */
    private fun sentenceHtml(text: String, range: IntRange?): String {
        if (range == null || range.isEmpty() || range.first < 0 || range.last >= text.length) {
            return JapaneseText.escapeHtml(text).replace("\n", "<br>")
        }
        val before = JapaneseText.escapeHtml(text.substring(0, range.first))
        val inside = JapaneseText.escapeHtml(text.substring(range.first, range.last + 1))
        val after = JapaneseText.escapeHtml(text.substring(range.last + 1))
        return "$before<b>$inside</b>$after".replace("\n", "<br>")
    }

    /**
     * Definitions grouped by dictionary, in the list structure Yomitan's cards use, so existing
     * card styling for ".yomitan-glossary" keeps working.
     */
    private fun glossary(word: LookupResult, firstOnly: Boolean): String {
        val entries = if (firstOnly) {
            val first = word.entries.firstOrNull()?.dictionary
            word.entries.filter { it.dictionary == first }
        } else {
            word.entries
        }
        if (entries.isEmpty()) return ""
        return buildString {
            append("<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>")
            for (entry in entries) {
                val body = GlossaryRenderer.toHtml(entry.glossaryJson, GlossaryRenderer.Mode.FULL)
                if (body.isEmpty()) continue
                append("<li data-dictionary=\"").append(JapaneseText.escapeHtml(entry.dictionary))
                    .append("\"><i>(")
                val tags = entry.definitionTags.split(' ').filter { it.isNotBlank() }
                if (tags.isNotEmpty()) {
                    append(JapaneseText.escapeHtml(tags.joinToString(", "))).append(", ")
                }
                append(JapaneseText.escapeHtml(entry.dictionary)).append(")</i> ").append(body)
                    .append("</li>")
            }
            append("</ol></div>")
        }
    }

    // ---- Field mapping ----

    /** Parses a stored mapping: a JSON object of field name to template. */
    fun parseMapping(json: String): Map<String, String> {
        if (json.isBlank()) return emptyMap()
        return try {
            val root = JSONObject(json)
            val out = LinkedHashMap<String, String>()
            for (key in root.keys()) out[key] = root.optString(key)
            out
        } catch (_: Exception) {
            emptyMap()
        }
    }

    fun mappingToJson(mapping: Map<String, String>): String =
        JSONObject(mapping as Map<*, *>).toString()

    /**
     * A first guess at templates for a note type, from common field names used by popular
     * Japanese mining note types. Fields that match nothing stay empty for the user to fill in.
     */
    fun suggestMapping(fields: List<String>): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (field in fields) {
            val name = field.lowercase().replace(Regex("[\\s_\\-]"), "")
            out[field] = when {
                name in setOf("expressionfurigana", "furigana", "vocabfurigana", "wordfurigana") ->
                    "{furigana}"

                name in setOf("expressionreading", "reading", "vocabreading", "wordreading", "kana", "vocabkana") ->
                    "{reading}"

                name in setOf("expression", "word", "vocab", "vocabulary", "vocabkanji", "key", "target", "term", "front") ->
                    "{expression}"

                name in setOf("sentencefurigana", "sentencereading") -> "{sentence-furigana}"

                name in setOf("sentence", "context", "example", "sentencekanji", "expressionsentence") ->
                    "{sentence}"

                name in setOf("sentenceaudio", "contextaudio") -> "{audio}"

                name in WORD_AUDIO_FIELDS -> "{word-audio}"

                name in setOf("picture", "image", "screenshot", "sentenceimage", "img") ->
                    "{screenshot}"

                name in setOf("maindefinition", "meaning", "glossary", "definition", "definitions", "vocabdef", "english", "selectiontext", "back") ->
                    "{glossary}"

                name in setOf("frequency", "frequencies", "freq") -> "{frequencies}"

                name in setOf("freqsort", "frequencysort", "freqrank") -> "{frequency-rank}"

                name in setOf("pitchposition", "pitchpositions", "pitchaccents", "pitch") ->
                    "{pitch-positions}"

                name in setOf("source", "miscinfo", "document", "sourcemedia", "game") -> "{source}"

                name in setOf("url", "link", "sourceurl") -> "{url}"

                else -> ""
            }
        }
        return out
    }
}
