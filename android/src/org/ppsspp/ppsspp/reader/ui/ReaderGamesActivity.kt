// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.LinearLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import com.google.android.material.button.MaterialButton
import java.io.File
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.ReaderCatalog
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ScriptStore

/**
 * The game scripts: the games whose text the reader can read (their scripts installed or
 * bundled, each with what it says of itself) and the scripts of the published list, to
 * install, update or remove; and scripts of the user's own, installed from files. In the game
 * list the games with a script carry the reader's badge.
 */
class ReaderGamesActivity : ReaderScreen() {
    override val titleRes = R.string.reader_games

    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var column: LinearLayout
    private lateinit var rows: LinearLayout
    private lateinit var status: android.widget.TextView
    private lateinit var folder: ScriptStore.Folder

    /** The list as last fetched; null before, or when it could not be had. */
    private var index: List<ScriptStore.Entry>? = null

    private val pick = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) installChosen(uris)
    }

    /** A game in the list on screen: what is installed of it and what the list has. */
    private data class Row(
        val id: String,
        val title: String,
        val reads: String,
        val version: String,
        val entry: ScriptStore.Entry?,
        val state: ScriptStore.State?,
        val user: Boolean,
        val bundled: Boolean,
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        folder = ScriptStore.Folder(File(getExternalFilesDir(null), ReaderNative.PROFILE_USER_DIR))
        column = scrollingColumn()
        column.addView(note(getString(R.string.reader_games_about)).apply { setPadding(0, dp(8f), 0, dp(4f)) })
        val actions = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        actions.addView(button(getString(R.string.reader_scripts_from_file)) {
            pick.launch(arrayOf("*/*"))
        })
        actions.addView(button(getString(R.string.reader_scripts_again)) { fetchIndex() })
        column.addView(actions)
        status = note("").apply { setPadding(0, dp(4f), 0, dp(4f)) }
        column.addView(status)
        rows = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(rows)
        refresh()
        fetchIndex()
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    private fun button(text: String, onClick: () -> Unit) =
        MaterialButton(this, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
            this.text = text
            isAllCaps = false
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams(WRAP, WRAP).apply { marginEnd = dp(8f) }
        }

    private fun fetchIndex() {
        status.text = getString(R.string.reader_scripts_fetching)
        worker.execute {
            val result = runCatching { ScriptStore.parseIndex(ScriptStore.fetcher(ScriptStore.indexUrl)) }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                result.onSuccess {
                    index = it
                    status.text = resources.getQuantityString(R.plurals.reader_scripts_listed, it.size, it.size)
                }.onFailure {
                    status.text = getString(R.string.reader_scripts_no_list, it.message ?: it.javaClass.simpleName)
                }
                refresh()
            }
        }
    }

    /** Reads what is installed again and lays the rows out. */
    private fun refresh() {
        worker.execute {
            ReaderCatalog.forget()
            val byId = LinkedHashMap<String, Row>()
            for (game in ReaderCatalog.games(applicationContext)) {
                byId[game.titleId] = Row(
                    game.titleId, game.title, game.reads, game.version, null, null,
                    user = game.own, bundled = ReaderNative.hasBundledProfile(game.titleId)
                )
            }
            for (entry in index.orEmpty()) {
                val bundled = bundledScript(entry.id)
                val user = folder.script(entry.id)
                byId[entry.id] = Row(
                    entry.id, entry.title, entry.reads, entry.version, entry,
                    ScriptStore.stateOf(entry, user, bundled, folder.recorded(entry.id)),
                    user = user != null, bundled = bundled != null
                )
            }
            val sorted = byId.values.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.title })
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                rows.removeAllViews()
                for (row in sorted) show(row)
            }
        }
    }

    private fun bundledScript(id: String): ByteArray? = try {
        assets.open("${ReaderNative.PROFILE_ASSET_DIR}/$id.lua").use { it.readBytes() }
    } catch (_: Exception) {
        null
    }

    private fun stateText(row: Row): String = when (row.state) {
        ScriptStore.State.NOT_INSTALLED -> getString(R.string.reader_scripts_state_not_installed)
        ScriptStore.State.INSTALLED -> getString(R.string.reader_scripts_state_installed)
        ScriptStore.State.UPDATE -> getString(R.string.reader_scripts_state_update)
        ScriptStore.State.OWN -> getString(R.string.reader_games_own)
        ScriptStore.State.TOO_NEW -> getString(R.string.reader_scripts_state_too_new)
        null -> when {
            row.user -> getString(if (index != null) R.string.reader_games_own else R.string.reader_scripts_state_installed)
            row.bundled -> getString(R.string.reader_scripts_state_bundled)
            else -> ""
        }
    }

    private fun show(row: Row) {
        rows.addView(
            body(row.title, 16f).apply {
                setTypeface(typeface, Typeface.BOLD)
                setPadding(0, dp(16f), 0, dp(2f))
            }
        )
        if (row.reads.isNotEmpty()) rows.addView(body(row.reads, 14f))
        val details = listOfNotNull(
            row.version.takeIf { it.isNotEmpty() },
            // A version line that names the game's ID already says it.
            row.id.takeIf { it != row.title && !row.version.replace("-", "").contains(it) },
            stateText(row).takeIf { it.isNotEmpty() }
        )
        if (details.isNotEmpty()) rows.addView(note(details.joinToString(" · ")))
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val entry = row.entry
        if (entry != null && row.state in setOf(ScriptStore.State.NOT_INSTALLED, ScriptStore.State.UPDATE, ScriptStore.State.OWN)) {
            val label = when (row.state) {
                ScriptStore.State.UPDATE -> R.string.reader_scripts_update
                ScriptStore.State.OWN -> R.string.reader_scripts_replace
                else -> R.string.reader_scripts_install
            }
            buttons.addView(button(getString(label)) {
                if (row.state == ScriptStore.State.OWN) {
                    AlertDialog.Builder(this)
                        .setMessage(getString(R.string.reader_scripts_replace_ask, entry.title))
                        .setPositiveButton(android.R.string.ok) { _, _ -> install(entry) }
                        .setNegativeButton(android.R.string.cancel, null)
                        .show()
                } else {
                    install(entry)
                }
            })
        }
        if (row.user) {
            buttons.addView(button(getString(R.string.reader_scripts_remove)) {
                val text = getString(R.string.reader_scripts_remove_ask, row.title) +
                    (if (row.bundled) " " + getString(R.string.reader_scripts_remove_bundled) else "")
                AlertDialog.Builder(this)
                    .setMessage(text)
                    .setPositiveButton(android.R.string.ok) { _, _ ->
                        worker.execute {
                            folder.remove(row.id)
                            runOnUiThread { changed(listOf(row.id), getString(R.string.reader_scripts_removed, row.title)) }
                        }
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            })
        }
        if (buttons.childCount > 0) rows.addView(buttons)
    }

    private fun install(entry: ScriptStore.Entry) {
        status.text = getString(R.string.reader_scripts_installing, entry.title)
        worker.execute {
            val result = runCatching {
                val files = (listOf(entry.script) + entry.data).associate {
                    it.file to ScriptStore.fetcher(ScriptStore.urlOf(it.file))
                }
                folder.install(entry, files)
            }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                result.onSuccess {
                    changed(listOf(entry.id), getString(R.string.reader_scripts_installed, entry.title))
                }.onFailure {
                    status.text = getString(R.string.reader_scripts_not_installed, entry.title, it.message.orEmpty())
                }
            }
        }
    }

    private fun installChosen(uris: List<Uri>) {
        worker.execute {
            val files = uris.mapNotNull { uri ->
                runCatching {
                    val name = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                        if (it.moveToFirst()) it.getString(0) else null
                    } ?: uri.lastPathSegment.orEmpty().substringAfterLast('/')
                    name to (contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: ByteArray(0))
                }.getOrNull()
            }
            val (ids, problems) = folder.installFiles(files)
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                val said = listOfNotNull(
                    ids.takeIf { it.isNotEmpty() }?.let { getString(R.string.reader_scripts_installed_files, it.joinToString(", ")) }
                ) + problems
                changed(ids, said.joinToString("\n"))
            }
        }
    }

    /** After an install or a removal: the game list's badges, and the running game's script at once. */
    private fun changed(ids: List<String>, message: String) {
        status.text = message
        ReaderCatalog.forget()
        if (ReaderSession.isActive && ReaderSession.gameKey in ids) {
            runCatching { ReaderNative.reloadProfile() }
        }
        refresh()
    }
}
