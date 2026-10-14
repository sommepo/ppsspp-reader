// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

import android.util.JsonReader
import android.util.JsonToken
import android.util.JsonWriter
import java.io.File
import java.io.InputStreamReader
import java.io.StringWriter
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import org.json.JSONObject

/**
 * Imports a dictionary archive in the Yomitan format (index.json plus term_bank_N.json and
 * term_meta_bank_N.json files) into a [DictionaryDatabase]. Kanji and tag banks are skipped.
 *
 * Banks are read as a token stream: a structured-content bank can be tens of megabytes and
 * would not fit in memory as an object tree.
 */
class DictionaryImporter(private val database: DictionaryDatabase) {
    class ImportException(message: String) : Exception(message)

    data class Result(val title: String, val terms: Long, val metas: Long)

    /**
     * Imports [archive]. [progress] is called with the number of banks done, the total and the
     * dictionary title; returning false from [keepGoing] abandons the import and leaves the
     * database unchanged.
     */
    fun import(
        archive: File,
        progress: (done: Int, total: Int, title: String) -> Unit = { _, _, _ -> },
        keepGoing: () -> Boolean = { true }
    ): Result {
        ZipFile(archive).use { zip ->
            val entries = zip.entries().toList().filter { !it.isDirectory }
            val indexEntry = entries
                .filter { it.name == "index.json" || it.name.endsWith("/index.json") }
                .minByOrNull { it.name.length }
                ?: throw ImportException("Not a dictionary archive: index.json is missing")
            val prefix = indexEntry.name.removeSuffix("index.json")
            val index = try {
                JSONObject(zip.getInputStream(indexEntry).use { it.readBytes().decodeToString() })
            } catch (e: Exception) {
                throw ImportException("index.json cannot be read: ${e.message}")
            }
            val title = index.optString("title").trim()
            if (title.isEmpty()) throw ImportException("index.json has no title")
            val format = index.optInt("format", index.optInt("version", 3))
            if (format != 1 && format != 3) {
                throw ImportException("Unsupported dictionary format $format")
            }

            fun banks(kind: String): List<ZipEntry> {
                val pattern = Regex(Regex.escape(prefix) + kind + "_(\\d+)\\.json")
                return entries.mapNotNull { e ->
                    pattern.matchEntire(e.name)?.let { m -> m.groupValues[1].toInt() to e }
                }.sortedBy { it.first }.map { it.second }
            }
            val termBanks = banks("term_bank")
            val metaBanks = banks("term_meta_bank")
            if (termBanks.isEmpty() && metaBanks.isEmpty()) {
                throw ImportException("\"$title\" has no term or frequency data")
            }
            val total = termBanks.size + metaBanks.size

            val db = database.writableDatabase
            if (db.rawQuery("SELECT 1 FROM dictionaries WHERE title = ?", arrayOf(title))
                    .use { it.moveToFirst() }
            ) {
                throw ImportException("\"$title\" is already installed")
            }

            var terms = 0L
            var metas = 0L
            db.beginTransaction()
            try {
                val priority = db.rawQuery(
                    "SELECT COALESCE(MAX(priority), -1) + 1 FROM dictionaries",
                    emptyArray()
                ).use { if (it.moveToFirst()) it.getInt(0) else 0 }
                db.execSQL(
                    "INSERT INTO dictionaries(title, revision, author, description, " +
                        "frequency_mode, priority) VALUES(?, ?, ?, ?, ?, ?)",
                    arrayOf<Any>(
                        title,
                        index.optString("revision"),
                        index.optString("author"),
                        index.optString("description"),
                        index.optString("frequencyMode"),
                        priority
                    )
                )
                val dictId = db.rawQuery("SELECT last_insert_rowid()", emptyArray())
                    .use { it.moveToFirst(); it.getLong(0) }

                val insertTerm = db.compileStatement(
                    "INSERT INTO terms(dict, expression, reading, def_tags, rules, score, " +
                        "glossary, sequence, term_tags) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"
                )
                val insertMeta = db.compileStatement(
                    "INSERT INTO term_meta(dict, expression, mode, data) VALUES(?, ?, ?, ?)"
                )
                var done = 0
                progress(done, total, title)

                for (entry in termBanks) {
                    if (!keepGoing()) throw ImportException("Import cancelled")
                    JsonReader(InputStreamReader(zip.getInputStream(entry), Charsets.UTF_8)).use {
                        it.beginArray()
                        while (it.hasNext()) {
                            it.beginArray()
                            val expression = it.nextString()
                            val reading = it.nextString()
                            val definitionTags = nextText(it)
                            val rules = nextText(it)
                            val score = nextNumber(it).toLong()
                            val glossary: String
                            var sequence = 0L
                            var termTags = ""
                            if (format >= 3) {
                                glossary = copyValue(it)
                                if (it.hasNext()) sequence = nextNumber(it).toLong()
                                if (it.hasNext()) termTags = nextText(it)
                            } else {
                                val text = StringWriter()
                                JsonWriter(text).use { w ->
                                    w.beginArray()
                                    while (it.hasNext()) w.value(nextText(it))
                                    w.endArray()
                                }
                                glossary = text.toString()
                            }
                            while (it.hasNext()) it.skipValue()
                            it.endArray()

                            insertTerm.bindLong(1, dictId)
                            insertTerm.bindString(2, expression)
                            // An empty reading means the expression is its own reading.
                            insertTerm.bindString(3, if (reading == expression) "" else reading)
                            insertTerm.bindString(4, definitionTags)
                            insertTerm.bindString(5, rules)
                            insertTerm.bindLong(6, score)
                            insertTerm.bindBlob(7, DictionaryDatabase.pack(glossary))
                            insertTerm.bindLong(8, sequence)
                            insertTerm.bindString(9, termTags)
                            insertTerm.executeInsert()
                            terms++
                        }
                        it.endArray()
                    }
                    progress(++done, total, title)
                }

                for (entry in metaBanks) {
                    if (!keepGoing()) throw ImportException("Import cancelled")
                    JsonReader(InputStreamReader(zip.getInputStream(entry), Charsets.UTF_8)).use {
                        it.beginArray()
                        while (it.hasNext()) {
                            it.beginArray()
                            val expression = it.nextString()
                            val mode = it.nextString()
                            val data = copyValue(it)
                            while (it.hasNext()) it.skipValue()
                            it.endArray()
                            insertMeta.bindLong(1, dictId)
                            insertMeta.bindString(2, expression)
                            insertMeta.bindString(3, mode)
                            insertMeta.bindString(4, data)
                            insertMeta.executeInsert()
                            metas++
                        }
                        it.endArray()
                    }
                    progress(++done, total, title)
                }

                db.execSQL(
                    "UPDATE dictionaries SET terms = ?, metas = ? WHERE id = ?",
                    arrayOf<Any>(terms, metas, dictId)
                )
                db.setTransactionSuccessful()
            } catch (e: ImportException) {
                throw e
            } catch (e: Exception) {
                throw ImportException("\"$title\" could not be imported: ${e.message}")
            } finally {
                db.endTransaction()
            }
            return Result(title, terms, metas)
        }
    }

    private fun nextText(reader: JsonReader): String = when (reader.peek()) {
        JsonToken.NULL -> {
            reader.nextNull()
            ""
        }

        JsonToken.STRING, JsonToken.NUMBER -> reader.nextString()

        else -> {
            reader.skipValue()
            ""
        }
    }

    private fun nextNumber(reader: JsonReader): Double = when (reader.peek()) {
        JsonToken.NUMBER -> reader.nextDouble()

        JsonToken.STRING -> reader.nextString().toDoubleOrNull() ?: 0.0

        else -> {
            reader.skipValue()
            0.0
        }
    }

    /** Reads the next JSON value, whatever its type, and returns it as JSON text. */
    private fun copyValue(reader: JsonReader): String {
        val text = StringWriter()
        JsonWriter(text).use { writer ->
            writer.isLenient = true // a meta value may be a bare number or string
            copy(reader, writer)
        }
        return text.toString()
    }

    private fun copy(reader: JsonReader, writer: JsonWriter) {
        when (reader.peek()) {
            JsonToken.BEGIN_ARRAY -> {
                reader.beginArray()
                writer.beginArray()
                while (reader.hasNext()) copy(reader, writer)
                reader.endArray()
                writer.endArray()
            }

            JsonToken.BEGIN_OBJECT -> {
                reader.beginObject()
                writer.beginObject()
                while (reader.hasNext()) {
                    writer.name(reader.nextName())
                    copy(reader, writer)
                }
                reader.endObject()
                writer.endObject()
            }

            JsonToken.STRING -> writer.value(reader.nextString())

            JsonToken.NUMBER -> {
                val number = reader.nextString()
                val asLong = number.toLongOrNull()
                if (asLong != null) writer.value(asLong) else writer.value(number.toDouble())
            }

            JsonToken.BOOLEAN -> writer.value(reader.nextBoolean())

            JsonToken.NULL -> {
                reader.nextNull()
                writer.nullValue()
            }

            else -> reader.skipValue()
        }
    }
}
