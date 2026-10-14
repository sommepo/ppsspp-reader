// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.anki

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import java.io.File

/**
 * AnkiDroid on this device, through its content provider (the same contract its "AddContentApi"
 * library uses). Needs AnkiDroid's READ_WRITE_DATABASE permission, which the user grants once.
 */
class AnkiDroidClient(private val context: Context) : AnkiClient {
    private class Model(val id: Long, val name: String, val fields: List<String>)

    private val resolver get() = context.contentResolver

    override fun deckNames(): List<String> = decks().keys.toList()

    override fun modelNames(): List<String> = models().map { it.name }

    override fun fieldNames(model: String): List<String> =
        models().firstOrNull { it.name == model }?.fields
            ?: throw AnkiException("AnkiDroid has no note type \"$model\"")

    override fun storeMedia(file: File, preferredName: String): String {
        val uri = FileProvider.getUriForFile(context, fileAuthority(context), file)
        context.grantUriPermission(ANKIDROID_PACKAGE, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        try {
            val values = ContentValues().apply {
                put("file_uri", uri.toString())
                put("preferred_name", preferredName.substringBeforeLast('.').replace(' ', '_'))
            }
            val stored = guard { resolver.insert(MEDIA_URI, values) }
                ?: throw AnkiException("AnkiDroid did not accept the media file")
            // The provider answers with a file URI whose path is the stored name.
            return File(stored.path ?: preferredName).name
        } finally {
            context.revokeUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    override fun addNote(
        deck: String,
        model: String,
        fields: Map<String, String>,
        tags: List<String>,
        duplicates: Duplicates
    ): Long {
        val noteType = models().firstOrNull { it.name == model }
            ?: throw AnkiException("AnkiDroid has no note type \"$model\"")
        val deckId = decks()[deck] ?: throw AnkiException("AnkiDroid has no deck \"$deck\"")
        val ordered = noteType.fields.map { fields[it] ?: "" }
        if (ordered.firstOrNull().isNullOrBlank()) {
            throw AnkiException("The first field (${noteType.fields.firstOrNull()}) would be empty")
        }
        // AnkiDroid adds whatever it is given: looking for the note first is up to this side.
        if (duplicates != Duplicates.ALLOW) {
            val scope = if (duplicates == Duplicates.REFUSE_IN_DECK) deck else null
            val existing = try {
                findNotes(scope, noteType.fields[0], ordered[0])
            } catch (_: AnkiException) {
                emptyList() // a search AnkiDroid cannot run is not a reason to refuse the card
            }
            if (existing.isNotEmpty()) {
                throw AnkiException("A note with this ${noteType.fields[0]} already exists")
            }
        }
        val values = ContentValues().apply {
            put("mid", noteType.id)
            put("flds", ordered.joinToString(FIELD_SEPARATOR))
            put("tags", tags.joinToString(" ") { it.trim().replace(' ', '_') })
        }
        val target = NOTES_URI.buildUpon().appendQueryParameter("deckId", deckId.toString()).build()
        val created = guard { resolver.insert(target, values) }
            ?: throw AnkiException("AnkiDroid did not create the note")
        return created.lastPathSegment?.toLongOrNull() ?: 0L
    }

    override fun findNotes(deck: String?, field: String, value: String): List<Long> {
        // The provider takes a card browser search where a database would take a selection.
        val query = AnkiSearch.fieldIs(deck, field, value)
        val cursor = guard { resolver.query(NOTES_URI, arrayOf("_id"), query, null, null) }
            ?: return emptyList()
        return cursor.use { c ->
            val out = ArrayList<Long>(c.count)
            while (c.moveToNext()) out.add(c.getLong(0))
            out
        }
    }

    override fun showNotes(noteIds: List<Long>) {
        if (noteIds.isEmpty()) return
        // AnkiDroid opens its card browser for this address (the same one AnkiConnect Android
        // uses). The flags keep repeated calls from stacking browsers on top of each other.
        val address = Uri.parse("anki://x-callback-url/browser")
            .buildUpon().appendQueryParameter("search", AnkiSearch.notes(noteIds)).build()
        val intent = Intent(Intent.ACTION_VIEW, address)
            .setPackage(ANKIDROID_PACKAGE)
            .addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or
                    Intent.FLAG_ACTIVITY_TASK_ON_HOME
            )
        try {
            context.startActivity(intent)
        } catch (e: Exception) {
            throw AnkiException("This AnkiDroid cannot show a note from another app", e)
        }
    }

    private fun decks(): Map<String, Long> {
        val out = LinkedHashMap<String, Long>()
        val cursor = guard { resolver.query(DECKS_URI, arrayOf("deck_name", "deck_id"), null, null, null) }
            ?: throw AnkiException("AnkiDroid did not answer. Open AnkiDroid once, then try again.")
        cursor.use { c ->
            val name = c.getColumnIndexOrThrow("deck_name")
            val id = c.getColumnIndexOrThrow("deck_id")
            while (c.moveToNext()) out[c.getString(name)] = c.getLong(id)
        }
        return out
    }

    private fun models(): List<Model> {
        val out = ArrayList<Model>()
        val cursor = guard {
            resolver.query(MODELS_URI, arrayOf("_id", "name", "field_names"), null, null, null)
        } ?: throw AnkiException("AnkiDroid did not answer. Open AnkiDroid once, then try again.")
        cursor.use { c ->
            val id = c.getColumnIndexOrThrow("_id")
            val name = c.getColumnIndexOrThrow("name")
            val fields = c.getColumnIndexOrThrow("field_names")
            while (c.moveToNext()) {
                out.add(Model(c.getLong(id), c.getString(name), c.getString(fields).split(FIELD_SEPARATOR)))
            }
        }
        return out
    }

    private inline fun <T> guard(block: () -> T): T = try {
        block()
    } catch (e: SecurityException) {
        throw AnkiException("AnkiDroid has not granted access yet", e)
    } catch (e: IllegalArgumentException) {
        throw AnkiException("AnkiDroid is not installed or too old", e)
    } catch (e: Exception) {
        throw AnkiException("AnkiDroid refused the request: ${e.message}", e)
    }

    companion object {
        const val ANKIDROID_PACKAGE = "com.ichi2.anki"
        const val PERMISSION = "com.ichi2.anki.permission.READ_WRITE_DATABASE"
        private const val AUTHORITY = "com.ichi2.anki.flashcards"
        private const val FIELD_SEPARATOR = "\u001f"
        private val NOTES_URI: Uri = Uri.parse("content://$AUTHORITY/notes")
        private val MODELS_URI: Uri = Uri.parse("content://$AUTHORITY/models")
        private val DECKS_URI: Uri = Uri.parse("content://$AUTHORITY/decks")
        private val MEDIA_URI: Uri = Uri.parse("content://$AUTHORITY/media")

        fun fileAuthority(context: Context) = context.packageName + ".reader.files"

        fun isInstalled(context: Context): Boolean =
            context.packageManager.resolveContentProvider(AUTHORITY, 0) != null

        fun hasPermission(context: Context): Boolean =
            ContextCompat.checkSelfPermission(context, PERMISSION) == PackageManager.PERMISSION_GRANTED
    }
}
