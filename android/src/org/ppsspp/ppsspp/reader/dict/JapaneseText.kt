// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

/** Small Japanese text helpers used by dictionary lookup and card building. */
object JapaneseText {
    fun isHiragana(c: Char) = c in 'ぁ'..'ゖ' || c == 'ゝ' || c == 'ゞ'

    fun isKatakana(c: Char) = c in 'ァ'..'ヺ' || c == 'ヽ' || c == 'ヾ'

    /** Kana of either script, including the long-vowel mark. */
    fun isKana(c: Char) = isHiragana(c) || isKatakana(c) || c == 'ー'

    fun isKanji(codePoint: Int) = codePoint in 0x4E00..0x9FFF || codePoint in 0x3400..0x4DBF ||
        codePoint in 0xF900..0xFAFF || codePoint in 0x20000..0x2FA1F || codePoint == 0x3005 ||
        codePoint == 0x3007

    /** True for characters a dictionary term can contain: kana, kanji, letters and digits. */
    fun isTermChar(codePoint: Int): Boolean = isKanji(codePoint) ||
        (codePoint <= 0xFFFF && isKana(codePoint.toChar())) ||
        Character.isLetterOrDigit(codePoint)

    fun katakanaToHiragana(text: String): String {
        var changed = false
        val out = CharArray(text.length)
        for (i in text.indices) {
            val c = text[i]
            if (c in 'ァ'..'ヶ' || c == 'ヽ' || c == 'ヾ') {
                out[i] = c - 0x60
                changed = true
            } else {
                out[i] = c
            }
        }
        return if (changed) String(out) else text
    }

    fun hiraganaToKatakana(text: String): String {
        var changed = false
        val out = CharArray(text.length)
        for (i in text.indices) {
            val c = text[i]
            if (c in 'ぁ'..'ゖ' || c == 'ゝ' || c == 'ゞ') {
                out[i] = c + 0x60
                changed = true
            } else {
                out[i] = c
            }
        }
        return if (changed) String(out) else text
    }

    /** Full-width ASCII letters and digits to their ordinary forms; other text unchanged. */
    fun fullWidthToAscii(text: String): String {
        var changed = false
        val out = CharArray(text.length)
        for (i in text.indices) {
            val c = text[i]
            if (c in '！'..'～') {
                out[i] = c - 0xFEE0
                changed = true
            } else {
                out[i] = c
            }
        }
        return if (changed) String(out) else text
    }

    /** A piece of a term: [text] with the [reading] to print above it, or null for kana. */
    data class FuriganaSegment(val text: String, val reading: String?)

    /**
     * Splits [expression] into segments so that readings sit only over the kanji parts:
     * 食べ物 / たべもの gives 食(た) べ 物(もの). Kana inside the expression anchors the split.
     * When the kana cannot be matched against the reading, the whole reading goes over the whole
     * expression.
     */
    fun distributeFurigana(expression: String, reading: String): List<FuriganaSegment> {
        if (reading.isEmpty() || reading == expression) {
            return listOf(FuriganaSegment(expression, null))
        }
        val normalizedReading = katakanaToHiragana(reading)
        // Runs of kana and runs of everything else.
        val groups = ArrayList<Pair<String, Boolean>>()
        var start = 0
        while (start < expression.length) {
            val kana = isKana(expression[start])
            var end = start + 1
            while (end < expression.length && isKana(expression[end]) == kana) end++
            groups.add(expression.substring(start, end) to kana)
            start = end
        }
        if (groups.all { it.second }) return listOf(FuriganaSegment(expression, null))

        val result = match(groups, 0, normalizedReading, reading, 0)
        return result ?: listOf(FuriganaSegment(expression, reading))
    }

    private fun match(
        groups: List<Pair<String, Boolean>>,
        index: Int,
        normalized: String,
        original: String,
        position: Int
    ): List<FuriganaSegment>? {
        if (index == groups.size) {
            return if (position == normalized.length) emptyList() else null
        }
        val (text, kana) = groups[index]
        if (kana) {
            val plain = katakanaToHiragana(text)
            if (!normalized.startsWith(plain, position)) return null
            val rest = match(groups, index + 1, normalized, original, position + plain.length)
                ?: return null
            return listOf(FuriganaSegment(text, null)) + rest
        }
        // A non-kana group takes at least one reading character; try the shortest split first.
        val last = index == groups.size - 1
        val from = position + 1
        val to = normalized.length
        var end = if (last) to else from
        while (end <= to) {
            val rest = match(groups, index + 1, normalized, original, end)
            if (rest != null) {
                return listOf(FuriganaSegment(text, original.substring(position, end))) + rest
            }
            end++
        }
        return null
    }

    /** [distributeFurigana] in Anki's syntax: " 食[た]べ 物[もの]". */
    fun ankiFurigana(expression: String, reading: String): String = buildString {
        for (segment in distributeFurigana(expression, reading)) {
            if (segment.reading == null) {
                append(segment.text)
            } else {
                if (isNotEmpty()) append(' ')
                append(segment.text).append('[').append(segment.reading).append(']')
            }
        }
    }

    /** [distributeFurigana] as HTML ruby. */
    fun rubyHtml(expression: String, reading: String): String = buildString {
        for (segment in distributeFurigana(expression, reading)) {
            if (segment.reading == null) {
                append(escapeHtml(segment.text))
            } else {
                append("<ruby>").append(escapeHtml(segment.text)).append("<rt>")
                    .append(escapeHtml(segment.reading)).append("</rt></ruby>")
            }
        }
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
