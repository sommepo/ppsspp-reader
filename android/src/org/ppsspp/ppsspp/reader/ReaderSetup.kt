// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import org.ppsspp.ppsspp.reader.dict.DictionaryStore

/**
 * What the reader's pages of the app's setup screen count as done (fragments/SetupFragment:
 * save sync, then a dictionary, then Anki; each can be passed). A page that is done shows
 * that instead of its button.
 */
object ReaderSetup {
    /** A dictionary is installed: a word can be looked up. */
    fun hasDictionary(context: Context): Boolean =
        runCatching { DictionaryStore.get(context).database.dictionaries().isNotEmpty() }.getOrDefault(false)

    /** Cards have somewhere to go: a deck and a note type were chosen. */
    fun hasAnki(context: Context): Boolean = ReaderSettings(context).run {
        ankiTransport != ReaderSettings.ANKI_NONE && ankiDeck.isNotEmpty() && ankiModel.isNotEmpty()
    }
}
