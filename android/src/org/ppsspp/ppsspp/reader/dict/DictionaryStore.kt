// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

import android.content.Context

/** The app's one dictionary database and the lookup built on it. */
class DictionaryStore private constructor(context: Context) {
    val database = DictionaryDatabase(context)
    val lookup = DictionaryLookup(database)

    companion object {
        @Volatile
        private var instance: DictionaryStore? = null

        fun get(context: Context): DictionaryStore = instance ?: synchronized(this) {
            instance ?: DictionaryStore(context.applicationContext).also { instance = it }
        }
    }
}
