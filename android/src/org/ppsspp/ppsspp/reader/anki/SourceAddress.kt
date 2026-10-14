// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.anki

import org.ppsspp.ppsspp.reader.dict.JapaneseText

/**
 * A card's source as a web address.
 *
 * A card made with Yomitan in a browser carries the page's address ({url}); a game has no
 * page of that kind, but it has one at its maker's: Nintendo's address for a program by its
 * ID. That is what the marker {url} gives here, so a note format that maps a field to {url}
 * means the same in both.
 *
 * It matters for cards that go to Anki on a computer through VN Library's media add-on (the
 * way from a handheld into a desktop collection): the add-on refuses a note whose source
 * field holds no web address, because for VN Library's own cards that address is how it finds
 * the picture and the sound. This app brings its own. When the add-on refuses a card for
 * that reason, the field it names gets the game's address, as a link around what the field
 * held, and the card is sent again: the user sets nothing up for it.
 */
object SourceAddress {
    /** The game's page at Nintendo, or nothing without a game's ID. */
    fun of(titleId: String): String {
        val id = titleId.trim().lowercase()
        return if (TITLE_ID.matches(id)) "https://www.jp.playstation.com/software/title/$id.html" else ""
    }

    /**
     * The field the add-on wants an address in, when [error] is its refusal for the lack of
     * one; null for anything else. The two sentences are the add-on's (vnkit/anki_bridge.py,
     * MediaBridge.enrich): an empty field, and one with text but no address.
     */
    fun askedFor(error: String?): String? {
        val text = error?.trim() ?: return null
        for (shape in REFUSALS) {
            shape.find(text)?.let { return it.groupValues[1].takeIf(String::isNotBlank) }
        }
        return null
    }

    fun hasAddress(value: String): Boolean = ADDRESS.containsMatchIn(value)

    /**
     * [fields] with [field] holding [url]: as a link around what the field holds, or around
     * the game's [title] when it holds nothing. A field that has an address already is left.
     * A note type without such a field is not harmed: Anki ignores a field it does not have.
     */
    fun with(fields: Map<String, String>, field: String, url: String, title: String): Map<String, String> {
        val held = fields[field].orEmpty()
        if (url.isEmpty() || hasAddress(held)) return fields
        val shown = held.ifBlank { JapaneseText.escapeHtml(title) }.ifBlank { JapaneseText.escapeHtml(url) }
        return fields + (field to "<a href=\"${JapaneseText.escapeHtml(url)}\">$shown</a>")
    }

    /** A note's id, and the field that had to be given an address for it (to remember). */
    data class Added(val id: Long, val field: String?)

    /**
     * Adds the note. [known] is the field an earlier card had to be given an address in; it
     * gets one at once. A refusal for the lack of an address is answered once, by giving the
     * named field the address and sending again.
     */
    fun addNote(
        client: AnkiClient,
        deck: String,
        model: String,
        fields: Map<String, String>,
        tags: List<String>,
        duplicates: Duplicates,
        url: String,
        title: String,
        known: String?
    ): Added {
        val first = if (known.isNullOrEmpty()) fields else with(fields, known, url, title)
        try {
            return Added(client.addNote(deck, model, first, tags, duplicates), known?.takeIf { first !== fields })
        } catch (e: AnkiException) {
            val field = askedFor(e.message) ?: throw e
            val second = with(first, field, url, title)
            if (second === first) throw e // nothing to give, or it has one and is refused all the same
            return Added(client.addNote(deck, model, second, tags, duplicates), field)
        }
    }

    private val TITLE_ID = Regex("[a-z]{4}[0-9]{5}")
    private val ADDRESS = Regex("https?://[^\\s<>\"']+")
    private val REFUSALS = listOf(
        Regex("^Map (.+?) to \\{url\\} in Yomitan"),
        Regex("^The (.+?) field must contain Yomitan.s \\{url\\}")
    )
}
