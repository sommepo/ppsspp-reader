// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.text.NumberFormat
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.ReadingRules
import org.ppsspp.ppsspp.reader.stats.SessionRecord
import org.ppsspp.ppsspp.reader.store.GameTotals
import org.ppsspp.ppsspp.reader.store.ReaderDatabase

/**
 * Reading statistics: today, the running game, every game, the last reading days and sessions,
 * with export and clearing. Statistics stay on this device.
 */
class ReaderStatsActivity : ReaderScreen() {
    override val titleRes = R.string.reader_statistics

    private class Snapshot(
        val today: ReadingCounters,
        val game: ReadingCounters?,
        val total: ReadingCounters,
        val days: List<Pair<String, ReadingCounters>>,
        val games: List<GameTotals>,
        val sessions: List<SessionRecord>
    )

    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var db: ReaderDatabase
    private lateinit var column: LinearLayout
    private val numbers = NumberFormat.getIntegerInstance()
    private val sessionTime = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")
    private val clock = DateTimeFormatter.ofPattern("HH:mm")
    private var pendingExport: String? = null

    private val saveDocument =
        registerForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri: Uri? ->
            val text = pendingExport
            pendingExport = null
            if (uri != null && text != null) write(uri, text)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        db = ReaderSession.database(this)
        column = scrollingColumn()
    }

    override fun onResume() {
        super.onResume()
        // Counts still in memory are written first, so the screen shows the present moment.
        ReaderSession.flush { load() }
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    private fun load() {
        val game = ReaderSession.gameKey.takeIf { it.isNotEmpty() }
        val today = ReadingRules.readingDay(System.currentTimeMillis())
        worker.execute {
            val snapshot = Snapshot(
                today = db.dayTotals(today),
                game = game?.let { db.totals(it) },
                total = db.totals(),
                days = db.dailyHistory(limit = 30),
                games = db.gameTotals(),
                sessions = db.sessions().take(MAX_SESSIONS)
            )
            runOnUiThread { if (!isDestroyed) render(snapshot, game) }
        }
    }

    private fun render(s: Snapshot, game: String?) {
        column.removeAllViews()
        if (s.total.isEmpty && s.sessions.isEmpty()) {
            column.addView(body(getString(R.string.reader_stats_none)).apply { setPadding(0, dp(24f), 0, 0) })
            return
        }

        column.addView(heading(getString(R.string.reader_stats_today)))
        column.addView(summary(s.today))
        if (s.game != null) {
            column.addView(heading(ReaderSession.gameTitle.ifEmpty { getString(R.string.reader_stats_this_game) }))
            column.addView(summary(s.game))
        }
        column.addView(heading(getString(R.string.reader_stats_total)))
        column.addView(summary(s.total))

        if (s.days.isNotEmpty()) {
            column.addView(heading(getString(R.string.reader_stats_days)))
            val most = s.days.maxOf { it.second.characters }.coerceAtLeast(1)
            for ((day, counters) in s.days) column.addView(dayRow(day, counters, most))
        }

        if (s.games.isNotEmpty()) {
            column.addView(heading(getString(R.string.reader_stats_games)))
            for (g in s.games) {
                column.addView(body(g.title))
                column.addView(note(oneLine(g.counters)).apply { setPadding(0, 0, 0, dp(8f)) })
            }
        }

        if (s.sessions.isNotEmpty()) {
            val titles = s.games.associate { it.game to it.title }
            column.addView(heading(getString(R.string.reader_stats_sessions)))
            for (session in s.sessions) column.addView(sessionRow(session, titles))
        }

        column.addView(spacer(16f))
        column.addView(action(R.string.reader_stats_export_json) {
            export("ppsspp-reader-activity.json") { db.exportActivityJson().toString(2) }
        })
        column.addView(action(R.string.reader_stats_export_csv) {
            export("ppsspp-reader-sessions.csv") { db.exportSessionsCsv() }
        })
        if (game != null) {
            column.addView(action(R.string.reader_stats_clear_game) { confirmClear(game) })
        }
        column.addView(action(R.string.reader_stats_clear_all) { confirmClear(null) })
    }

    // ---- Rows ----

    private fun summary(c: ReadingCounters): View {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row.addView(figure(numbers.format(c.characters), getString(R.string.reader_stats_characters)), weight())
        row.addView(figure(duration(c.activeMs), getString(R.string.reader_stats_time)), weight())
        row.addView(
            figure(c.charactersPerHour?.let { numbers.format(it.toLong()) } ?: "–", getString(R.string.reader_stats_speed)),
            weight()
        )
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        box.addView(row)
        box.addView(
            note(
                "${getString(R.string.reader_stats_unique)} ${numbers.format(c.uniqueCharacters)}   " +
                    "${getString(R.string.reader_stats_reread)} ${numbers.format(c.rereadCharacters)}   " +
                    "${getString(R.string.reader_stats_lines)} ${numbers.format(c.lines)}"
            ).apply { setPadding(0, dp(4f), 0, 0) }
        )
        return box
    }

    private fun figure(value: String, caption: String): View {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        box.addView(body(value, 22f))
        box.addView(note(caption))
        return box
    }

    private fun weight() = LinearLayout.LayoutParams(0, WRAP, 1f)

    private fun dayRow(day: String, c: ReadingCounters, most: Long): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2f), 0, dp(2f))
        }
        row.addView(note(day.substring(5)), LinearLayout.LayoutParams(dp(52f), WRAP))
        val track = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val share = c.characters.toFloat() / most
        track.addView(
            View(this).apply {
                setBackgroundColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorPrimary))
            },
            LinearLayout.LayoutParams(0, dp(10f), share.coerceAtLeast(0.004f))
        )
        track.addView(View(this), LinearLayout.LayoutParams(0, dp(10f), 1f - share))
        row.addView(track, LinearLayout.LayoutParams(0, WRAP, 1f))
        row.addView(
            note("${numbers.format(c.characters)} · ${duration(c.activeMs)}").apply { gravity = Gravity.END },
            LinearLayout.LayoutParams(dp(132f), WRAP)
        )
        return row
    }

    private fun sessionRow(session: SessionRecord, titles: Map<String, String>): View {
        val zone = ZoneId.systemDefault()
        val start = Instant.ofEpochMilli(session.started).atZone(zone)
        val end = Instant.ofEpochMilli(session.ended ?: session.lastActivity).atZone(zone)
        val open = if (session.ended == null) " (${getString(R.string.reader_stats_open_session)})" else ""
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(6f), 0, dp(6f))
        }
        row.addView(body("${sessionTime.format(start)} – ${clock.format(end)}$open"))
        row.addView(note("${titles[session.game] ?: session.game} · ${oneLine(session.counters)}"))
        row.isLongClickable = true
        row.setOnLongClickListener {
            confirmDelete(session)
            true
        }
        row.setOnClickListener { confirmDelete(session) }
        return row
    }

    private fun action(label: Int, onClick: () -> Unit) = Button(this, null, android.R.attr.borderlessButtonStyle).apply {
        setText(label)
        isAllCaps = false
        gravity = Gravity.START or Gravity.CENTER_VERTICAL
        setOnClickListener { onClick() }
    }

    private fun oneLine(c: ReadingCounters): String {
        val speed = c.charactersPerHour?.let { " · ${numbers.format(it.toLong())}/h" } ?: ""
        return "${numbers.format(c.characters)} · ${duration(c.activeMs)}$speed"
    }

    // ---- Actions ----

    private fun confirmDelete(session: SessionRecord) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.reader_stats_delete_session)
            .setMessage(R.string.reader_stats_delete_session_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                worker.execute {
                    db.deleteSession(session.id)
                    runOnUiThread {
                        ReaderSession.onActivityCleared()
                        load()
                    }
                }
            }
            .show()
    }

    private fun confirmClear(game: String?) {
        MaterialAlertDialogBuilder(this)
            .setTitle(if (game == null) R.string.reader_stats_clear_all else R.string.reader_stats_clear_game)
            .setMessage(R.string.reader_stats_clear_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                worker.execute {
                    db.clearActivity(game)
                    runOnUiThread {
                        ReaderSession.onActivityCleared()
                        load()
                    }
                }
            }
            .show()
    }

    private fun export(name: String, produce: () -> String) {
        worker.execute {
            val text = produce()
            runOnUiThread {
                pendingExport = text
                saveDocument.launch(name)
            }
        }
    }

    private fun write(uri: Uri, text: String) {
        worker.execute {
            val error = try {
                contentResolver.openOutputStream(uri, "wt")!!.use { it.write(text.toByteArray(Charsets.UTF_8)) }
                null
            } catch (e: Exception) {
                e.message ?: e.javaClass.simpleName
            }
            runOnUiThread {
                val message = if (error == null) {
                    getString(R.string.reader_stats_exported)
                } else {
                    getString(R.string.reader_stats_export_failed, error)
                }
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            }
        }
    }

    companion object {
        private const val MAX_SESSIONS = 100

        /** "1h 05m", "12m 30s" or "45s". */
        fun duration(ms: Long): String {
            val seconds = ms / 1000
            val h = seconds / 3600
            val m = seconds % 3600 / 60
            val s = seconds % 60
            return when {
                h > 0 -> String.format("%dh %02dm", h, m)
                m > 0 -> String.format("%dm %02ds", m, s)
                else -> "${s}s"
            }
        }
    }
}
