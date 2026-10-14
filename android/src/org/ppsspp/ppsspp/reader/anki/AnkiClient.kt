// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.anki

import java.io.File

/** Something went wrong talking to Anki; the message is shown to the user. */
class AnkiException(message: String, cause: Throwable? = null) : Exception(message, cause)

/** What Anki itself is asked to do about a note whose first field it already has. */
enum class Duplicates { ALLOW, REFUSE_IN_DECK, REFUSE_ANYWHERE }

/**
 * The few Anki operations card creation needs, over either AnkiDroid on this device or
 * AnkiConnect (Anki on a computer, or AnkiConnect Android). All calls block and may throw
 * [AnkiException]: use a worker thread.
 */
interface AnkiClient {
    fun deckNames(): List<String>

    fun modelNames(): List<String>

    fun fieldNames(model: String): List<String>

    /**
     * Copies [file] into the collection's media. Returns the name it was stored under, which
     * may differ from [preferredName].
     */
    fun storeMedia(file: File, preferredName: String): String

    /**
     * Adds a note and returns its id. [fields] is keyed by field name. Not every Anki looks for
     * duplicates when asked to ([duplicates]): callers that must not add one use [findNotes].
     */
    fun addNote(
        deck: String,
        model: String,
        fields: Map<String, String>,
        tags: List<String>,
        duplicates: Duplicates
    ): Long

    /**
     * Ids of the notes whose [field] holds exactly [value]: those with a card in [deck] or one
     * of its subdecks, or in the whole collection when [deck] is null.
     */
    fun findNotes(deck: String?, field: String, value: String): List<Long>

    /** Opens Anki's card browser on [noteIds]. */
    fun showNotes(noteIds: List<Long>)
}

/** Search strings in the syntax of Anki's card browser. */
object AnkiSearch {
    /** Notes whose [field] is exactly [value], within [deck] and its subdecks unless null. */
    fun fieldIs(deck: String?, field: String, value: String): String = buildString {
        if (deck != null) append('"').append("deck:").append(escape(deck)).append("\" ")
        // A colon in the field's name would end the name early; one in the value is harmless.
        append('"').append(escape(field).replace(":", "\\:")).append(':').append(escape(value)).append('"')
    }

    fun notes(noteIds: List<Long>): String = "nid:" + noteIds.joinToString(",")

    /** [text] for use inside a quoted term: nothing in it acts as a wildcard or ends the quote. */
    fun escape(text: String): String = buildString(text.length + 8) {
        for (c in text) {
            if (c == '\\' || c == '"' || c == '*' || c == '_') append('\\')
            append(c)
        }
    }
}
