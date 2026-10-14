// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import java.io.File
import java.text.NumberFormat
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryInfo
import org.ppsspp.ppsspp.reader.dict.DictionaryStore

/**
 * Installed dictionaries: import Yomitan archives, choose their order (the first is shown first
 * in lookups), switch them on and off, delete them.
 */
class DictionaryManagerActivity : ReaderScreen() {
    override val titleRes = R.string.reader_dictionaries

    private val worker = Executors.newSingleThreadExecutor()
    private val cancelled = AtomicBoolean(false)
    private lateinit var store: DictionaryStore
    private lateinit var list: LinearLayout
    private lateinit var status: TextView
    private lateinit var importButton: Button
    private lateinit var cancelButton: Button
    private val numbers = NumberFormat.getIntegerInstance()

    private val pickFiles =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (!uris.isNullOrEmpty()) import(uris)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = DictionaryStore.get(this)
        val column = scrollingColumn()

        importButton = Button(this).apply {
            setText(R.string.reader_dict_import)
            isAllCaps = false
            setOnClickListener {
                pickFiles.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
            }
        }
        cancelButton = Button(this, null, android.R.attr.borderlessButtonStyle).apply {
            setText(R.string.reader_dict_cancel)
            isAllCaps = false
            visibility = View.GONE
            setOnClickListener { cancelled.set(true) }
        }
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.addView(importButton)
        buttons.addView(cancelButton)
        column.addView(buttons)

        status = note("").apply { setPadding(0, dp(8f), 0, dp(8f)) }
        column.addView(status)

        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(list)
        reload()
    }

    override fun onDestroy() {
        cancelled.set(true)
        worker.shutdown()
        super.onDestroy()
    }

    private fun reload() {
        worker.execute {
            val dictionaries = store.database.dictionaries()
            runOnUiThread { if (!isDestroyed) render(dictionaries) }
        }
    }

    private fun render(dictionaries: List<DictionaryInfo>) {
        list.removeAllViews()
        if (dictionaries.isEmpty()) {
            list.addView(body(getString(R.string.reader_dict_none)))
            return
        }
        dictionaries.forEachIndexed { index, info ->
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, dp(8f), 0, dp(8f))
            }
            val text = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            text.addView(body(info.title))
            text.addView(
                note(
                    getString(R.string.reader_dict_entry_counts, info.termCount, info.metaCount) +
                        if (info.revision.isNotEmpty()) " · ${info.revision}" else ""
                )
            )
            row.addView(text, LinearLayout.LayoutParams(0, WRAP, 1f))

            row.addView(
                icon(R.drawable.ic_reader_down, R.string.reader_dict_move_up, enabled = index > 0) {
                    move(dictionaries, index, index - 1)
                }.apply { rotation = 180f }
            )
            row.addView(
                icon(R.drawable.ic_reader_down, R.string.reader_dict_move_down, enabled = index < dictionaries.lastIndex) {
                    move(dictionaries, index, index + 1)
                }
            )
            row.addView(icon(R.drawable.ic_reader_x_delete, R.string.reader_dict_delete, enabled = true) { confirmDelete(info) })
            row.addView(
                MaterialSwitch(this).apply {
                    isChecked = info.enabled
                    setOnCheckedChangeListener { _, checked ->
                        worker.execute { store.database.setEnabled(info.id, checked) }
                    }
                }
            )
            list.addView(row)
        }
    }

    private fun icon(drawable: Int, description: Int, enabled: Boolean, action: () -> Unit) =
        ImageButton(this, null, android.R.attr.borderlessButtonStyle).apply {
            setImageResource(drawable)
            imageTintList = android.content.res.ColorStateList.valueOf(
                MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurface)
            )
            contentDescription = getString(description)
            isEnabled = enabled
            alpha = if (enabled) 1f else 0.3f
            layoutParams = LinearLayout.LayoutParams(dp(44f), dp(44f))
            setOnClickListener { action() }
        }

    private fun move(dictionaries: List<DictionaryInfo>, from: Int, to: Int) {
        val ids = dictionaries.map { it.id }.toMutableList()
        ids.add(to, ids.removeAt(from))
        worker.execute {
            store.database.setOrder(ids)
            reload()
        }
    }

    private fun confirmDelete(info: DictionaryInfo) {
        MaterialAlertDialogBuilder(this)
            .setMessage(getString(R.string.reader_dict_delete_confirm, info.title))
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.reader_dict_delete) { _, _ ->
                worker.execute {
                    store.database.delete(info.id)
                    reload()
                }
            }
            .show()
    }

    private fun import(uris: List<Uri>) {
        cancelled.set(false)
        setImporting(true)
        worker.execute {
            val messages = ArrayList<String>()
            val importer = DictionaryImporter(store.database)
            val folder = File(cacheDir, "reader/import").apply { mkdirs() }
            for (uri in uris) {
                if (cancelled.get()) break
                val copy = File(folder, "dictionary.zip")
                try {
                    show(getString(R.string.reader_dict_copying))
                    contentResolver.openInputStream(uri)!!.use { input ->
                        copy.outputStream().use { input.copyTo(it) }
                    }
                    val result = importer.import(
                        copy,
                        progress = { done, total, title ->
                            show(getString(R.string.reader_dict_importing, title, done, total))
                        },
                        keepGoing = { !cancelled.get() }
                    )
                    messages.add(
                        getString(
                            R.string.reader_dict_imported,
                            result.title,
                            result.terms.toInt(),
                            result.metas.toInt()
                        )
                    )
                } catch (e: Exception) {
                    messages.add(getString(R.string.reader_dict_failed, e.message ?: e.javaClass.simpleName))
                } finally {
                    copy.delete()
                }
            }
            val dictionaries = store.database.dictionaries()
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                setImporting(false)
                status.text = messages.joinToString("\n")
                render(dictionaries)
            }
        }
    }

    private fun show(message: String) {
        runOnUiThread { if (!isDestroyed) status.text = message }
    }

    private fun setImporting(importing: Boolean) {
        importButton.isEnabled = !importing
        cancelButton.visibility = if (importing) View.VISIBLE else View.GONE
        if (importing) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }
}
