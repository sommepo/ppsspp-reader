// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.model

import org.json.JSONArray
import org.json.JSONObject

/** A reading written above part of a line. Offsets count Unicode code points of [TextLine.text]. */
data class RubySpan(val start: Int, val length: Int, val reading: String)

/**
 * One piece of text the game presented, as reported by the text engine.
 *
 * @property sourceId Stable identity of this text within the game. Two presentations of the same
 * source are the same text read again.
 * @property audioPosition Position in the emulated audio history when the line appeared.
 * @property timestamp Wall-clock milliseconds when the line reached the app.
 * @property window What the game shows the text in, when its profile reports where text is on
 * screen ([ScreenLayout]); empty otherwise.
 */
data class TextLine(
    val sequence: Long,
    val frame: Long,
    val audioPosition: Long,
    val channel: String,
    val speaker: String,
    val text: String,
    val ruby: List<RubySpan>,
    val sourceId: String,
    val meta: Map<String, String>,
    val timestamp: Long,
    val window: String = ""
) {
    val isChoice: Boolean get() = channel == CHANNEL_CHOICE

    /** Text with readings in Anki's furigana syntax: " 漢字[かんじ]". */
    fun toAnkiFurigana(): String = buildString {
        forEachSegment { base, reading ->
            if (reading == null) {
                append(base)
            } else {
                if (isNotEmpty() && !last().isWhitespace()) append(' ')
                append(base).append('[').append(reading).append(']')
            }
        }
    }

    /** Text as HTML with ruby elements. Line breaks become br elements. */
    fun toRubyHtml(): String = buildString {
        forEachSegment { base, reading ->
            val escaped = escapeHtml(base).replace("\n", "<br>")
            if (reading == null) {
                append(escaped)
            } else {
                append("<ruby>").append(escaped).append("<rt>")
                    .append(escapeHtml(reading)).append("</rt></ruby>")
            }
        }
    }

    /** Calls [block] for each run of text, with its reading when the run carries ruby. */
    inline fun forEachSegment(block: (base: String, reading: String?) -> Unit) {
        var cursor = 0 // UTF-16 index
        var codePoint = 0
        for (span in ruby.sortedBy { it.start }) {
            if (span.start < codePoint) continue // overlapping spans: keep the first
            val start = text.offsetByCodePointsSafe(cursor, span.start - codePoint)
            val end = text.offsetByCodePointsSafe(start, span.length)
            if (start > cursor) block(text.substring(cursor, start), null)
            if (end > start) block(text.substring(start, end), span.reading)
            cursor = end
            codePoint = span.start + span.length
        }
        if (cursor < text.length) block(text.substring(cursor), null)
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("seq", sequence)
        put("frame", frame)
        put("audio", audioPosition)
        put("channel", channel)
        put("speaker", speaker)
        put("text", text)
        put("ruby", rubyToJson(ruby))
        put("id", sourceId)
        put("meta", JSONObject(meta as Map<*, *>))
        put("time", timestamp)
        if (window.isNotEmpty()) put("window", window)
    }

    companion object {
        const val CHANNEL_DIALOGUE = "dialogue"
        const val CHANNEL_CHOICE = "choice"

        /** Parses the engine's event JSON. Returns null when the text is missing or empty. */
        fun fromJson(json: String, timestamp: Long): TextLine? {
            val root = try {
                JSONObject(json)
            } catch (_: Exception) {
                return null
            }
            val text = root.optString("text")
            if (text.isEmpty()) return null
            val meta = LinkedHashMap<String, String>()
            root.optJSONObject("meta")?.let { m ->
                for (key in m.keys()) meta[key] = m.get(key).toString()
            }
            return TextLine(
                sequence = root.optLong("seq"),
                frame = root.optLong("frame"),
                audioPosition = root.optLong("audio"),
                channel = root.optString("channel").ifEmpty { CHANNEL_DIALOGUE },
                speaker = root.optString("speaker"),
                text = text,
                ruby = rubyFromJson(root.optJSONArray("ruby"), text),
                sourceId = root.optString("id"),
                meta = meta,
                timestamp = root.optLong("time", timestamp),
                window = root.optString("window")
            )
        }

        fun rubyToJson(ruby: List<RubySpan>): JSONArray = JSONArray().apply {
            for (span in ruby) {
                put(JSONArray().put(span.start).put(span.length).put(span.reading))
            }
        }

        /** Parses ruby spans, dropping any that fall outside [text]. */
        fun rubyFromJson(array: JSONArray?, text: String): List<RubySpan> {
            if (array == null) return emptyList()
            val codePoints = text.codePointCount(0, text.length)
            val out = ArrayList<RubySpan>(array.length())
            for (i in 0 until array.length()) {
                val item = array.optJSONArray(i) ?: continue
                val start = item.optInt(0, -1)
                val length = item.optInt(1, 0)
                val reading = item.optString(2)
                if (start < 0 || length <= 0 || reading.isEmpty()) continue
                if (start + length > codePoints) continue
                out.add(RubySpan(start, length, reading))
            }
            return out
        }

        fun escapeHtml(s: String): String = buildString(s.length) {
            for (c in s) {
                when (c) {
                    '&' -> append("&amp;")
                    '<' -> append("&lt;")
                    '>' -> append("&gt;")
                    '"' -> append("&quot;")
                    else -> append(c)
                }
            }
        }
    }
}

/** Like [String.offsetByCodePoints] but clamps to the end of the string instead of throwing. */
fun String.offsetByCodePointsSafe(index: Int, codePointOffset: Int): Int {
    var i = index.coerceIn(0, length)
    var remaining = codePointOffset
    while (remaining > 0 && i < length) {
        i += Character.charCount(codePointAt(i))
        remaining--
    }
    return i.coerceAtMost(length)
}
