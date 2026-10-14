// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

import org.json.JSONArray
import org.json.JSONObject

/**
 * Turns a dictionary entry's glossary (plain strings or Yomitan "structured content") into HTML.
 *
 * Two flavours: [Mode.FULL] keeps the structure (lists, tables, ruby, inline styles and the
 * data attributes dictionary stylesheets rely on) and is what goes into Anki cards;
 * [Mode.SIMPLE] only uses what Android's Html.fromHtml can show, for the lookup popup.
 * Images are left out in both.
 */
object GlossaryRenderer {
    enum class Mode { FULL, SIMPLE }

    /** The definitions of one glossary, each as an HTML fragment. */
    fun definitions(glossaryJson: String, mode: Mode): List<String> {
        val array = try {
            JSONArray(glossaryJson)
        } catch (_: Exception) {
            return emptyList()
        }
        val out = ArrayList<String>(array.length())
        for (i in 0 until array.length()) {
            val html = StringBuilder()
            when (val item = array.opt(i)) {
                is String -> appendText(html, item)

                is JSONObject -> when (item.optString("type")) {
                    "text" -> appendText(html, item.optString("text"))
                    "structured-content" -> render(item.opt("content"), mode, html, ListState())
                    else -> Unit // images
                }

                else -> Unit // deinflection pointers [term, [reasons]] carry no definition
            }
            val text = trimBreaks(html.toString())
            if (text.isNotEmpty()) out.add(text)
        }
        return out
    }

    /** One glossary as a single HTML fragment: a list when it has several definitions. */
    fun toHtml(glossaryJson: String, mode: Mode): String {
        val items = definitions(glossaryJson, mode)
        return when {
            items.isEmpty() -> ""

            items.size == 1 -> items[0]

            mode == Mode.FULL -> items.joinToString("", "<ul>", "</ul>") { "<li>$it</li>" }

            else -> items.joinToString("<br>") { "• $it" }
        }
    }

    /** The glossary as plain text, definitions separated by "; ". */
    fun toPlainText(glossaryJson: String): String = definitions(glossaryJson, Mode.SIMPLE)
        .joinToString("; ") { stripTags(it) }

    fun stripTags(html: String): String = html
        .replace(Regex("<br\\s*/?>|</div>|</p>"), "\n")
        .replace(Regex("<[^>]+>"), "")
        .replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&amp;", "&")
        .replace(Regex("\n{2,}"), "\n")
        .trim()

    /** A definition cut down to one line: its glosses, and remarks about how the word is used. */
    class Compact(val glosses: String, val notes: String)

    /**
     * The definitions of one glossary as short plain text, for a popup that is to stay small.
     * Where the dictionary marks what is what (JMdict does), only the glosses and the notes are
     * kept: cross-references, example sentences and tables of forms are left out. A dictionary
     * that marks nothing is kept whole, its blocks run together.
     */
    fun compact(glossaryJson: String): List<Compact> {
        val array = try {
            JSONArray(glossaryJson)
        } catch (_: Exception) {
            return emptyList()
        }
        val out = ArrayList<Compact>(array.length())
        for (i in 0 until array.length()) {
            val definition = when (val item = array.opt(i)) {
                is String -> Compact(oneLine(item), "")

                is JSONObject -> when (item.optString("type")) {
                    "text" -> Compact(oneLine(item.optString("text")), "")
                    "structured-content" -> compactContent(item.opt("content"))
                    else -> null // images
                }

                else -> null
            }
            if (definition != null && definition.glosses.isNotEmpty()) out.add(definition)
        }
        return out
    }

    private val noteMarks = setOf("notes", "infoGlossary", "sourceLanguages")

    private fun compactContent(content: Any?): Compact {
        val glosses = ArrayList<Any>()
        val notes = ArrayList<Any>()
        collectMarked(content, glosses, notes)
        if (glosses.isEmpty()) return Compact(oneLine(plain(content)), "")
        return Compact(
            glosses.joinToString("; ") { oneLine(plain(it)) },
            notes.joinToString("; ") { oneLine(plain(it)) }
        )
    }

    /** Finds the parts a dictionary marked as glosses or as notes, wherever they are nested. */
    private fun collectMarked(node: Any?, glosses: MutableList<Any>, notes: MutableList<Any>) {
        when (node) {
            is JSONArray -> for (i in 0 until node.length()) collectMarked(node.opt(i), glosses, notes)

            is JSONObject -> when (val mark = node.optJSONObject("data")?.optString("content").orEmpty()) {
                "glossary" -> glosses.add(node)
                in noteMarks -> notes.add(node)
                "" -> collectMarked(node.opt("content"), glosses, notes)
                else -> Unit // marked as something the compact popup leaves out
            }
        }
    }

    private val blockTags = setOf(
        "li", "div", "p", "ul", "ol", "tr", "table", "details", "summary", "thead", "tbody", "tfoot"
    )

    /** The text of structured content, one line per block. */
    private fun plain(node: Any?): String = StringBuilder().also { plain(node, it) }.toString()

    private fun plain(node: Any?, out: StringBuilder) {
        when (node) {
            null, JSONObject.NULL -> Unit

            is String -> out.append(node)

            is Number, is Boolean -> out.append(node.toString())

            is JSONArray -> for (i in 0 until node.length()) plain(node.opt(i), out)

            is JSONObject -> when (val tag = node.optString("tag")) {
                "br" -> out.append('\n')

                "img", "rt", "rp" -> Unit

                in blockTags -> {
                    out.append('\n')
                    plain(node.opt("content"), out)
                    out.append('\n')
                }

                else -> {
                    plain(node.opt("content"), out)
                    if (tag == "td" || tag == "th") out.append(' ')
                }
            }
        }
    }

    /** Lines run together with "; ", without the space around them. */
    private fun oneLine(text: String): String =
        text.split('\n').map { it.trim() }.filter { it.isNotEmpty() }.joinToString("; ")

    /** Removes whitespace and line breaks from both ends of a fragment. */
    private fun trimBreaks(html: String): String {
        var text = html.trim()
        while (true) {
            val shorter = text.removePrefix("<br>").removeSuffix("<br>").trim()
            if (shorter == text) return text
            text = shorter
        }
    }

    private class ListState(val ordered: Boolean = false, var index: Int = 0, val depth: Int = 0)

    private fun appendText(out: StringBuilder, text: String) {
        out.append(JapaneseText.escapeHtml(text).replace("\n", "<br>"))
    }

    private fun render(node: Any?, mode: Mode, out: StringBuilder, list: ListState) {
        when (node) {
            null, JSONObject.NULL -> Unit

            is String -> appendText(out, node)

            is Number, is Boolean -> out.append(node.toString())

            is JSONArray -> for (i in 0 until node.length()) render(node.opt(i), mode, out, list)

            is JSONObject -> element(node, mode, out, list)
        }
    }

    private fun element(node: JSONObject, mode: Mode, out: StringBuilder, list: ListState) {
        val tag = node.optString("tag")
        val content = node.opt("content")
        if (mode == Mode.FULL) {
            fullElement(tag, node, content, out, list)
        } else {
            simpleElement(tag, node, content, out, list)
        }
    }

    // ---- Full HTML ----

    private val fullTags = setOf(
        "ruby", "rt", "rp", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "span", "div",
        "ol", "ul", "li", "details", "summary", "a"
    )

    private fun fullElement(
        tag: String,
        node: JSONObject,
        content: Any?,
        out: StringBuilder,
        list: ListState
    ) {
        when (tag) {
            "br" -> out.append("<br>")

            "img" -> Unit

            in fullTags -> {
                out.append('<').append(tag)
                node.optJSONObject("style")?.let { style ->
                    val css = styleToCss(style)
                    if (css.isNotEmpty()) {
                        out.append(" style=\"").append(JapaneseText.escapeHtml(css)).append('"')
                    }
                }
                node.optJSONObject("data")?.let { data ->
                    for (key in data.keys()) {
                        val name = key.replace(Regex("[^A-Za-z0-9_\\-]"), "-")
                        out.append(" data-sc-").append(name).append("=\"")
                            .append(JapaneseText.escapeHtml(data.optString(key))).append('"')
                    }
                }
                for (attribute in arrayOf("lang", "title")) {
                    if (node.has(attribute)) {
                        out.append(' ').append(attribute).append("=\"")
                            .append(JapaneseText.escapeHtml(node.optString(attribute))).append('"')
                    }
                }
                if (tag == "td" || tag == "th") {
                    for (attribute in arrayOf("colSpan", "rowSpan")) {
                        if (node.has(attribute)) {
                            out.append(' ').append(attribute.lowercase()).append("=\"")
                                .append(node.optInt(attribute, 1)).append('"')
                        }
                    }
                }
                if (tag == "a") {
                    val href = node.optString("href")
                    // Only real links are useful outside the dictionary viewer.
                    if (href.startsWith("http://") || href.startsWith("https://")) {
                        out.append(" href=\"").append(JapaneseText.escapeHtml(href)).append('"')
                    }
                }
                out.append('>')
                render(content, Mode.FULL, out, list)
                out.append("</").append(tag).append('>')
            }

            else -> render(content, Mode.FULL, out, list)
        }
    }

    private fun styleToCss(style: JSONObject): String = buildString {
        for (key in style.keys()) {
            val value = style.opt(key) ?: continue
            if (value is JSONObject || value is JSONArray) continue
            val property = key.replace(Regex("[A-Z]")) { "-" + it.value.lowercase() }
            if (!property.matches(Regex("[a-z\\-]+"))) continue
            val text = value.toString()
            if (text.contains(';') || text.contains('{') || text.contains('}')) continue
            append(property).append(": ").append(text).append("; ")
        }
    }.trim()

    // ---- Html.fromHtml subset ----

    private fun simpleElement(
        tag: String,
        node: JSONObject,
        content: Any?,
        out: StringBuilder,
        list: ListState
    ) {
        when (tag) {
            "br" -> out.append("<br>")

            "img", "rt", "rp" -> Unit

            "ruby" -> render(content, Mode.SIMPLE, out, list)

            "div", "details" -> {
                block(out)
                styled(node, content, out, list)
                block(out)
            }

            "summary" -> {
                out.append("<b>")
                render(content, Mode.SIMPLE, out, list)
                out.append("</b>")
                block(out)
            }

            "ol", "ul" -> {
                val inner = ListState(ordered = tag == "ol", depth = list.depth + 1)
                block(out)
                render(content, Mode.SIMPLE, out, inner)
                block(out)
            }

            "li" -> {
                block(out)
                repeat((list.depth - 1).coerceAtLeast(0)) { out.append("&nbsp;&nbsp;") }
                list.index++
                out.append(if (list.ordered) "${list.index}. " else "• ")
                styled(node, content, out, ListState(list.ordered, 0, list.depth))
            }

            "table", "thead", "tbody", "tfoot" -> render(content, Mode.SIMPLE, out, list)

            "tr" -> {
                block(out)
                render(content, Mode.SIMPLE, out, list)
                block(out)
            }

            "td", "th" -> {
                if (tag == "th") out.append("<b>")
                render(content, Mode.SIMPLE, out, list)
                if (tag == "th") out.append("</b>")
                out.append(" &nbsp;")
            }

            else -> styled(node, content, out, list)
        }
    }

    /** Separates blocks with exactly one line break. */
    private fun block(out: StringBuilder) {
        if (out.isEmpty()) return
        if (!out.endsWith("<br>")) out.append("<br>")
    }

    private fun styled(node: JSONObject, content: Any?, out: StringBuilder, list: ListState) {
        val style = node.optJSONObject("style")
        val bold = style?.optString("fontWeight") == "bold"
        val italic = style?.optString("fontStyle") == "italic"
        val underline = style?.optString("textDecorationLine")?.contains("underline") == true
        val small = style?.optString("fontSize")?.let {
            it == "small" || it == "x-small" || it.removeSuffix("em").toDoubleOrNull()?.let { s -> s < 0.9 } == true
        } == true
        if (bold) out.append("<b>")
        if (italic) out.append("<i>")
        if (underline) out.append("<u>")
        if (small) out.append("<small>")
        render(content, Mode.SIMPLE, out, list)
        if (small) out.append("</small>")
        if (underline) out.append("</u>")
        if (italic) out.append("</i>")
        if (bold) out.append("</b>")
    }
}
