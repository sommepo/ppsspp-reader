// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import java.io.ByteArrayOutputStream
import java.util.zip.Deflater
import java.util.zip.Inflater

/** An installed dictionary. Lower [priority] is shown first. */
data class DictionaryInfo(
    val id: Long,
    val title: String,
    val revision: String,
    val author: String,
    val description: String,
    val frequencyMode: String,
    val priority: Int,
    val enabled: Boolean,
    val termCount: Long,
    val metaCount: Long
)

/** One dictionary row for a term. [glossaryJson] is the entry's glossary array as JSON. */
data class TermEntry(
    val dictionaryId: Long,
    val dictionary: String,
    val priority: Int,
    val expression: String,
    val reading: String,
    val definitionTags: String,
    val rules: String,
    val score: Int,
    val glossaryJson: String,
    val sequence: Long,
    val termTags: String
)

/** A frequency, pitch or other "meta" row for a term. [data] is its JSON value. */
data class TermMeta(
    val dictionary: String,
    val frequencyMode: String,
    val expression: String,
    val mode: String,
    val data: String
)

/**
 * Dictionaries imported from Yomitan-format archives, stored for lookup by expression or reading.
 * Glossaries are kept compressed: structured-content dictionaries are several hundred megabytes
 * as plain JSON.
 */
class DictionaryDatabase(context: Context, name: String? = DATABASE_NAME) :
    SQLiteOpenHelper(context, name, null, VERSION) {

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE dictionaries (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT NOT NULL UNIQUE,
                revision TEXT NOT NULL,
                author TEXT NOT NULL,
                description TEXT NOT NULL,
                frequency_mode TEXT NOT NULL,
                priority INTEGER NOT NULL,
                enabled INTEGER NOT NULL DEFAULT 1,
                terms INTEGER NOT NULL DEFAULT 0,
                metas INTEGER NOT NULL DEFAULT 0
            )"""
        )
        db.execSQL(
            """CREATE TABLE terms (
                id INTEGER PRIMARY KEY,
                dict INTEGER NOT NULL,
                expression TEXT NOT NULL,
                reading TEXT NOT NULL,
                def_tags TEXT NOT NULL,
                rules TEXT NOT NULL,
                score INTEGER NOT NULL,
                glossary BLOB NOT NULL,
                sequence INTEGER NOT NULL,
                term_tags TEXT NOT NULL
            )"""
        )
        db.execSQL("CREATE INDEX terms_expression ON terms(expression)")
        db.execSQL("CREATE INDEX terms_reading ON terms(reading)")
        db.execSQL(
            """CREATE TABLE term_meta (
                id INTEGER PRIMARY KEY,
                dict INTEGER NOT NULL,
                expression TEXT NOT NULL,
                mode TEXT NOT NULL,
                data TEXT NOT NULL
            )"""
        )
        db.execSQL("CREATE INDEX term_meta_expression ON term_meta(expression)")
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        // Version 1 is the only schema so far.
    }

    fun dictionaries(): List<DictionaryInfo> {
        val out = ArrayList<DictionaryInfo>()
        readableDatabase.rawQuery(
            "SELECT id, title, revision, author, description, frequency_mode, priority, enabled, " +
                "terms, metas FROM dictionaries ORDER BY priority, id",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) {
                out.add(
                    DictionaryInfo(
                        c.getLong(0), c.getString(1), c.getString(2), c.getString(3),
                        c.getString(4), c.getString(5), c.getInt(6), c.getInt(7) != 0,
                        c.getLong(8), c.getLong(9)
                    )
                )
            }
        }
        return out
    }

    fun hasEnabledDictionaries(): Boolean = readableDatabase.rawQuery(
        "SELECT 1 FROM dictionaries WHERE enabled = 1 LIMIT 1",
        emptyArray()
    ).use { it.moveToFirst() }

    fun setEnabled(id: Long, enabled: Boolean) {
        writableDatabase.execSQL(
            "UPDATE dictionaries SET enabled = ? WHERE id = ?",
            arrayOf<Any>(if (enabled) 1 else 0, id)
        )
    }

    /** Stores a new display order: the first id becomes the highest priority. */
    fun setOrder(ids: List<Long>) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            ids.forEachIndexed { index, id ->
                db.execSQL(
                    "UPDATE dictionaries SET priority = ? WHERE id = ?",
                    arrayOf<Any>(index, id)
                )
            }
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    fun delete(id: Long) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            db.delete("terms", "dict = ?", arrayOf(id.toString()))
            db.delete("term_meta", "dict = ?", arrayOf(id.toString()))
            db.delete("dictionaries", "id = ?", arrayOf(id.toString()))
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    /** Term rows of enabled dictionaries whose expression or reading is one of [terms]. */
    fun findTerms(terms: Collection<String>): List<TermEntry> {
        if (terms.isEmpty()) return emptyList()
        val out = ArrayList<TermEntry>()
        val seen = HashSet<Long>()
        val db = readableDatabase
        for (chunk in terms.chunked(MAX_PARAMETERS)) {
            val marks = chunk.joinToString(",") { "?" }
            val args = chunk.toTypedArray()
            for (column in arrayOf("expression", "reading")) {
                db.rawQuery(
                    """SELECT t.id, t.dict, d.title, d.priority, t.expression, t.reading,
                              t.def_tags, t.rules, t.score, t.glossary, t.sequence, t.term_tags
                       FROM terms t JOIN dictionaries d ON d.id = t.dict
                       WHERE t.$column IN ($marks) AND d.enabled = 1""",
                    args
                ).use { c ->
                    while (c.moveToNext()) {
                        if (!seen.add(c.getLong(0))) continue
                        out.add(
                            TermEntry(
                                dictionaryId = c.getLong(1),
                                dictionary = c.getString(2),
                                priority = c.getInt(3),
                                expression = c.getString(4),
                                reading = c.getString(5),
                                definitionTags = c.getString(6),
                                rules = c.getString(7),
                                score = c.getInt(8),
                                glossaryJson = unpack(c.getBlob(9)),
                                sequence = c.getLong(10),
                                termTags = c.getString(11)
                            )
                        )
                    }
                }
            }
        }
        return out
    }

    /** Frequency and pitch rows of enabled dictionaries for the given expressions. */
    fun findMeta(expressions: Collection<String>): List<TermMeta> {
        if (expressions.isEmpty()) return emptyList()
        val out = ArrayList<TermMeta>()
        val db = readableDatabase
        for (chunk in expressions.chunked(MAX_PARAMETERS)) {
            val marks = chunk.joinToString(",") { "?" }
            db.rawQuery(
                """SELECT d.title, d.frequency_mode, m.expression, m.mode, m.data
                   FROM term_meta m JOIN dictionaries d ON d.id = m.dict
                   WHERE m.expression IN ($marks) AND d.enabled = 1
                   ORDER BY d.priority, d.id""",
                chunk.toTypedArray()
            ).use { c ->
                while (c.moveToNext()) {
                    out.add(
                        TermMeta(
                            c.getString(0), c.getString(1), c.getString(2), c.getString(3),
                            c.getString(4)
                        )
                    )
                }
            }
        }
        return out
    }

    companion object {
        const val DATABASE_NAME = "reader_dict.db"
        private const val VERSION = 1
        private const val MAX_PARAMETERS = 400
        private const val PACK_RAW: Byte = 0
        private const val PACK_DEFLATE: Byte = 1
        private const val PACK_THRESHOLD = 96

        /** Compresses a glossary for storage; short ones are stored as they are. */
        fun pack(json: String): ByteArray {
            val raw = json.toByteArray(Charsets.UTF_8)
            if (raw.size < PACK_THRESHOLD) return byteArrayOf(PACK_RAW) + raw
            val deflater = Deflater(Deflater.BEST_SPEED)
            try {
                deflater.setInput(raw)
                deflater.finish()
                val out = ByteArrayOutputStream(raw.size / 2 + 16)
                out.write(PACK_DEFLATE.toInt())
                val buffer = ByteArray(4096)
                while (!deflater.finished()) {
                    out.write(buffer, 0, deflater.deflate(buffer))
                }
                return out.toByteArray()
            } finally {
                deflater.end()
            }
        }

        fun unpack(blob: ByteArray): String {
            if (blob.isEmpty()) return "[]"
            if (blob[0] == PACK_RAW) return String(blob, 1, blob.size - 1, Charsets.UTF_8)
            val inflater = Inflater()
            try {
                inflater.setInput(blob, 1, blob.size - 1)
                val out = ByteArrayOutputStream(blob.size * 4)
                val buffer = ByteArray(4096)
                while (!inflater.finished()) {
                    val n = inflater.inflate(buffer)
                    if (n == 0 && (inflater.needsInput() || inflater.needsDictionary())) break
                    out.write(buffer, 0, n)
                }
                return out.toString("UTF-8")
            } finally {
                inflater.end()
            }
        }
    }
}
