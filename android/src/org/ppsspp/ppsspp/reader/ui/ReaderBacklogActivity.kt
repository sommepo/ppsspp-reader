// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import org.ppsspp.ppsspp.reader.screen.Area
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.store.BacklogEntry
import org.ppsspp.ppsspp.reader.store.ReaderDatabase

/**
 * Every line read in a game, oldest at the top, searchable. Words can be looked up and made into
 * cards here as in the game; a card from an older line gets that line's kept screenshot and
 * audio when they are still held.
 */
class ReaderBacklogActivity : ReaderScreen() {
    override val titleRes = R.string.reader_backlog

    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var db: ReaderDatabase
    private lateinit var settings: ReaderSettings
    private lateinit var game: String
    private lateinit var listView: RecyclerView
    private lateinit var lookups: LookupLayer
    private lateinit var empty: TextView
    private val adapter = LineAdapter()
    private val time = DateTimeFormatter.ofPattern("MM-dd HH:mm")
    private var query = ""
    private var loading = false
    private var reachedStart = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        db = ReaderSession.database(this)
        settings = ReaderSettings(this)
        ReaderTextView.markStyle = settings.wordMark
        game = intent.getStringExtra(EXTRA_GAME) ?: ReaderSession.gameKey

        val search = EditText(this).apply {
            setHint(R.string.reader_backlog_search)
            isSingleLine = true
            setPadding(dp(16f), dp(8f), dp(16f), dp(8f))
            addTextChangedListener(object : TextWatcher {
                override fun afterTextChanged(s: Editable) {
                    query = s.toString().trim()
                    reload()
                }

                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            })
        }
        content.addView(search, LinearLayout.LayoutParams(MATCH, WRAP))

        val frame = FrameLayout(this)
        listView = RecyclerView(this).apply {
            layoutManager = LinearLayoutManager(this@ReaderBacklogActivity)
            adapter = this@ReaderBacklogActivity.adapter
            addOnScrollListener(object : RecyclerView.OnScrollListener() {
                override fun onScrolled(recyclerView: RecyclerView, dx: Int, dy: Int) {
                    val first = (layoutManager as LinearLayoutManager).findFirstVisibleItemPosition()
                    if (first in 0..4) loadOlder()
                }
            })
        }
        frame.addView(listView, FrameLayout.LayoutParams(MATCH, MATCH))
        empty = body(getString(R.string.reader_backlog_empty)).apply {
            setPadding(dp(16f), dp(24f), dp(16f), 0)
            visibility = View.GONE
        }
        frame.addView(empty)
        lookups = LookupLayer(this) {
            startActivity(android.content.Intent(this, DictionaryManagerActivity::class.java))
        }
        lookups.passThrough = listView
        frame.addView(lookups)
        content.addView(frame, LinearLayout.LayoutParams(MATCH, 0, 1f))

        reload()
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (lookups.isOpen) lookups.close() else super.onBackPressed()
    }

    /** Loads the newest page (or the search result) and scrolls to the latest line. */
    private fun reload() {
        val wanted = query
        loading = true
        worker.execute {
            val page = if (wanted.isEmpty()) {
                db.recentLines(game, PAGE)
            } else {
                db.searchLines(game, wanted, SEARCH_LIMIT)
            }
            runOnUiThread {
                if (isDestroyed || wanted != query) return@runOnUiThread
                loading = false
                reachedStart = wanted.isNotEmpty() || page.size < PAGE
                lookups.close()
                adapter.replace(page.asReversed())
                empty.visibility = if (page.isEmpty()) View.VISIBLE else View.GONE
                if (page.isNotEmpty()) listView.scrollToPosition(adapter.itemCount - 1)
            }
        }
    }

    private fun loadOlder() {
        if (loading || reachedStart || query.isNotEmpty()) return
        val oldest = adapter.oldestId() ?: return
        loading = true
        worker.execute {
            val page = db.recentLines(game, PAGE, beforeId = oldest)
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                loading = false
                reachedStart = page.size < PAGE
                adapter.prepend(page.asReversed())
            }
        }
    }

    private inner class LineAdapter : RecyclerView.Adapter<LineHolder>() {
        private val items = ArrayList<BacklogEntry>()

        fun replace(entries: List<BacklogEntry>) {
            items.clear()
            items.addAll(entries)
            notifyDataSetChanged()
        }

        fun prepend(entries: List<BacklogEntry>) {
            if (entries.isEmpty()) return
            items.addAll(0, entries)
            notifyItemRangeInserted(0, entries.size)
        }

        fun oldestId(): Long? = items.firstOrNull()?.id

        override fun getItemCount() = items.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): LineHolder {
            val row = LinearLayout(parent.context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(16f), dp(8f), dp(16f), dp(8f))
                layoutParams = RecyclerView.LayoutParams(MATCH, WRAP)
            }
            val caption = note("")
            val text = ReaderTextView(parent.context).apply {
                setTextSize(TypedValue.COMPLEX_UNIT_SP, settings.fontSizeSp * 0.9f)
                setLineSpacing(0f, 1.1f)
                wordHighlightColor = ReaderStyle.HIGHLIGHT
            }
            row.addView(caption)
            row.addView(text)
            return LineHolder(row, caption, text)
        }

        override fun onBindViewHolder(holder: LineHolder, position: Int) {
            val entry = items[position]
            val line = entry.line
            val stamp = time.format(Instant.ofEpochMilli(line.timestamp).atZone(ZoneId.systemDefault()))
            holder.caption.text = if (line.speaker.isEmpty()) stamp else "${line.speaker} · $stamp"
            holder.text.setTextColor(
                MaterialColors.getColor(
                    holder.text,
                    if (entry.reread && settings.rereadColour) {
                        com.google.android.material.R.attr.colorError
                    } else {
                        com.google.android.material.R.attr.colorOnSurface
                    }
                )
            )
            holder.text.setLine(line.text, line.ruby, settings.showFurigana)
            // Lines of this run still know their audio position and may have a screenshot.
            val presented = ReaderSession.recentLine(entry.id)
                ?: PresentedLine(line, LineAccounting(entry.characters, entry.reread, true, null), entry.id)
            holder.text.onCharacterTap = { offset, bounds ->
                // Right next to the tapped word, above or below its row.
                val area = LookupLayer.areaOf(holder.text, lookups)
                val row = Area(area.left, area.top + bounds.top, area.right, area.top + bounds.bottom)
                lookups.lookup(holder.text, presented, offset, LookupLayer.Anchor(row, area.left + bounds.left))
            }
            holder.text.onLongPress = {
                ReaderSession.copyToClipboard(line.text)
                android.widget.Toast.makeText(
                    this@ReaderBacklogActivity,
                    R.string.reader_line_copied,
                    android.widget.Toast.LENGTH_SHORT
                ).show()
            }
        }
    }

    private class LineHolder(row: View, val caption: TextView, val text: ReaderTextView) :
        RecyclerView.ViewHolder(row)

    companion object {
        const val EXTRA_GAME = "game"
        private const val PAGE = 200
        private const val SEARCH_LIMIT = 500
    }
}
