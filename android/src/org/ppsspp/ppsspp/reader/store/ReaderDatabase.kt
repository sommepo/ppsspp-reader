// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.store

import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.stats.ActivityStore
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.ReadingRules
import org.ppsspp.ppsspp.reader.stats.SessionRecord
import org.json.JSONArray
import org.json.JSONObject

/** A line kept in the backlog. */
data class BacklogEntry(
    val id: Long,
    val game: String,
    val line: TextLine,
    val characters: Int,
    val reread: Boolean
)

/** Totals of one game, for the statistics screen. */
data class GameTotals(val game: String, val title: String, val counters: ReadingCounters)

/**
 * Reading history kept on this device: the backlog of presented lines, which sources were seen,
 * and reading activity per session and reading day.
 *
 * Activity is stored as one row per session and day, so a session, a day, a game or everything
 * can be totalled, and deleting a session subtracts exactly what it contributed. Clearing
 * activity never touches the backlog or the seen sources: text met again is still rereading.
 */
class ReaderDatabase(context: Context, name: String? = DATABASE_NAME) :
    SQLiteOpenHelper(context, name, null, VERSION),
    ActivityStore {

    override fun onConfigure(db: SQLiteDatabase) {
        db.setForeignKeyConstraintsEnabled(false)
    }

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE lines (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                game TEXT NOT NULL,
                session_id INTEGER,
                ts INTEGER NOT NULL,
                channel TEXT NOT NULL,
                speaker TEXT NOT NULL,
                text TEXT NOT NULL,
                ruby TEXT NOT NULL,
                source_id TEXT NOT NULL,
                meta TEXT NOT NULL,
                chars INTEGER NOT NULL,
                reread INTEGER NOT NULL
            )"""
        )
        db.execSQL("CREATE INDEX lines_game ON lines(game, id)")
        db.execSQL(
            """CREATE TABLE seen (
                game TEXT NOT NULL,
                source_id TEXT NOT NULL,
                first_ts INTEGER NOT NULL,
                count INTEGER NOT NULL,
                PRIMARY KEY (game, source_id)
            ) WITHOUT ROWID"""
        )
        db.execSQL(
            """CREATE TABLE sessions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                game TEXT NOT NULL,
                started INTEGER NOT NULL,
                last_activity INTEGER NOT NULL,
                ended INTEGER
            )"""
        )
        db.execSQL("CREATE INDEX sessions_game ON sessions(game, id)")
        db.execSQL(
            """CREATE TABLE daily (
                session_id INTEGER NOT NULL,
                day TEXT NOT NULL,
                game TEXT NOT NULL,
                active_ms INTEGER NOT NULL DEFAULT 0,
                chars INTEGER NOT NULL DEFAULT 0,
                unique_chars INTEGER NOT NULL DEFAULT 0,
                reread_chars INTEGER NOT NULL DEFAULT 0,
                lines INTEGER NOT NULL DEFAULT 0,
                choice_chars INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (session_id, day)
            ) WITHOUT ROWID"""
        )
        db.execSQL("CREATE INDEX daily_game ON daily(game, day)")
        db.execSQL("CREATE TABLE games (game TEXT PRIMARY KEY, title TEXT NOT NULL) WITHOUT ROWID")
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        // Version 2 counts Japanese characters only: the same tables, counted again.
        if (oldVersion < 2) recountCharacters(db)
    }

    /**
     * Counts every kept line again by today's rule and takes the difference out of the day it
     * was counted in, so that what was read before the rule changed is measured like what is
     * read after it. A line that no longer has anything to count is no longer a line read.
     */
    internal fun recountCharacters(db: SQLiteDatabase) {
        data class Change(val id: Long, val session: Long, val day: String, val choice: Boolean,
                          val reread: Boolean, val was: Int, val now: Int)
        val changes = ArrayList<Change>()
        db.rawQuery(
            "SELECT id, session_id, ts, channel, text, chars, reread FROM lines WHERE session_id IS NOT NULL",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) {
                val now = ReadingRules.countCharacters(c.getString(4))
                val was = c.getInt(5)
                if (now == was) continue
                changes.add(
                    Change(c.getLong(0), c.getLong(1), ReadingRules.readingDay(c.getLong(2)),
                        c.getString(3) == TextLine.CHANNEL_CHOICE, c.getInt(6) != 0, was, now)
                )
            }
        }
        for (change in changes) {
            val less = change.was - change.now
            db.execSQL("UPDATE lines SET chars = ? WHERE id = ?", arrayOf<Any>(change.now, change.id))
            if (change.choice) {
                db.execSQL(
                    "UPDATE daily SET choice_chars = MAX(0, choice_chars - ?) WHERE session_id = ? AND day = ?",
                    arrayOf<Any>(less, change.session, change.day)
                )
            } else {
                val kind = if (change.reread) "reread_chars" else "unique_chars"
                val gone = if (change.now == 0) 1 else 0
                db.execSQL(
                    "UPDATE daily SET chars = MAX(0, chars - ?), $kind = MAX(0, $kind - ?), " +
                        "lines = MAX(0, lines - ?) WHERE session_id = ? AND day = ?",
                    arrayOf<Any>(less, less, gone, change.session, change.day)
                )
            }
        }
    }

    // ---- Games ----

    fun setGameTitle(game: String, title: String) {
        writableDatabase.execSQL(
            "INSERT INTO games(game, title) VALUES(?, ?) " +
                "ON CONFLICT(game) DO UPDATE SET title = excluded.title",
            arrayOf(game, title)
        )
    }

    // ---- Backlog ----

    fun insertLine(game: String, line: TextLine, accounting: LineAccounting): Long {
        val values = ContentValues().apply {
            put("game", game)
            accounting.sessionId?.let { put("session_id", it) }
            put("ts", line.timestamp)
            put("channel", line.channel)
            put("speaker", line.speaker)
            put("text", line.text)
            put("ruby", TextLine.rubyToJson(line.ruby).toString())
            put("source_id", line.sourceId)
            put("meta", JSONObject(line.meta as Map<*, *>).toString())
            put("chars", accounting.characters)
            put("reread", if (accounting.reread) 1 else 0)
        }
        return writableDatabase.insert("lines", null, values)
    }

    /** The newest lines of [game], newest first, optionally only those older than [beforeId]. */
    fun recentLines(game: String, limit: Int, beforeId: Long? = null): List<BacklogEntry> {
        val (where, args) = if (beforeId == null) {
            "game = ?" to arrayOf(game)
        } else {
            "game = ? AND id < ?" to arrayOf(game, beforeId.toString())
        }
        return queryLines(where, args, limit)
    }

    /** Lines of [game] whose text or speaker contains [query], newest first. */
    fun searchLines(game: String, query: String, limit: Int): List<BacklogEntry> {
        val pattern = "%" + query.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"
        return queryLines(
            "game = ? AND (text LIKE ? ESCAPE '\\' OR speaker LIKE ? ESCAPE '\\')",
            arrayOf(game, pattern, pattern),
            limit
        )
    }

    fun lineCount(game: String): Long = readableDatabase.rawQuery(
        "SELECT COUNT(*) FROM lines WHERE game = ?",
        arrayOf(game)
    ).use { if (it.moveToFirst()) it.getLong(0) else 0 }

    private fun queryLines(where: String, args: Array<String>, limit: Int): List<BacklogEntry> {
        val out = ArrayList<BacklogEntry>()
        readableDatabase.query(
            "lines",
            arrayOf(
                "id", "game", "ts", "channel", "speaker", "text", "ruby", "source_id", "meta",
                "chars", "reread"
            ),
            where,
            args,
            null,
            null,
            "id DESC",
            limit.toString()
        ).use { c ->
            while (c.moveToNext()) {
                val text = c.getString(5)
                val meta = LinkedHashMap<String, String>()
                runCatching { JSONObject(c.getString(8)) }.getOrNull()?.let { m ->
                    for (key in m.keys()) meta[key] = m.get(key).toString()
                }
                val ruby = runCatching { JSONArray(c.getString(6)) }.getOrNull()
                out.add(
                    BacklogEntry(
                        id = c.getLong(0),
                        game = c.getString(1),
                        line = TextLine(
                            sequence = 0,
                            frame = 0,
                            audioPosition = -1,
                            channel = c.getString(3),
                            speaker = c.getString(4),
                            text = text,
                            ruby = TextLine.rubyFromJson(ruby, text),
                            sourceId = c.getString(7),
                            meta = meta,
                            timestamp = c.getLong(2)
                        ),
                        characters = c.getInt(9),
                        reread = c.getInt(10) != 0
                    )
                )
            }
        }
        return out
    }

    // ---- ActivityStore ----

    override fun openSession(game: String): SessionRecord? = readableDatabase.rawQuery(
        "SELECT id, game, started, last_activity, ended FROM sessions " +
            "WHERE game = ? AND ended IS NULL ORDER BY id DESC LIMIT 1",
        arrayOf(game)
    ).use { c -> if (c.moveToFirst()) sessionFrom(c, ReadingCounters()) else null }

    override fun createSession(game: String, started: Long): Long {
        val values = ContentValues().apply {
            put("game", game)
            put("started", started)
            put("last_activity", started)
        }
        return writableDatabase.insert("sessions", null, values)
    }

    override fun sessionCounters(id: Long): ReadingCounters =
        sumDaily("session_id = ?", arrayOf(id.toString()))

    override fun closeSession(id: Long, ended: Long) {
        writableDatabase.execSQL(
            "UPDATE sessions SET ended = ? WHERE id = ? AND ended IS NULL",
            arrayOf<Any>(ended, id)
        )
    }

    override fun addActivity(
        sessionId: Long,
        game: String,
        day: String,
        delta: ReadingCounters,
        lastActivity: Long
    ) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            db.execSQL(
                """INSERT INTO daily(session_id, day, game, active_ms, chars, unique_chars,
                       reread_chars, lines, choice_chars)
                   VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
                   ON CONFLICT(session_id, day) DO UPDATE SET
                       active_ms = active_ms + excluded.active_ms,
                       chars = chars + excluded.chars,
                       unique_chars = unique_chars + excluded.unique_chars,
                       reread_chars = reread_chars + excluded.reread_chars,
                       lines = lines + excluded.lines,
                       choice_chars = choice_chars + excluded.choice_chars""",
                arrayOf<Any>(
                    sessionId, day, game, delta.activeMs, delta.characters,
                    delta.uniqueCharacters, delta.rereadCharacters, delta.lines,
                    delta.choiceCharacters
                )
            )
            db.execSQL(
                "UPDATE sessions SET last_activity = MAX(last_activity, ?) WHERE id = ?",
                arrayOf<Any>(lastActivity, sessionId)
            )
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    override fun sessionSources(id: Long): Collection<String> {
        val out = ArrayList<String>()
        readableDatabase.rawQuery(
            "SELECT DISTINCT source_id FROM lines WHERE session_id = ? AND source_id != ''",
            arrayOf(id.toString())
        ).use { c ->
            while (c.moveToNext()) out.add(c.getString(0))
        }
        return out
    }

    override fun markSeen(game: String, sourceId: String, timestamp: Long): Boolean {
        val db = writableDatabase
        val seenBefore = db.rawQuery(
            "SELECT 1 FROM seen WHERE game = ? AND source_id = ?",
            arrayOf(game, sourceId)
        ).use { it.moveToFirst() }
        db.execSQL(
            "INSERT INTO seen(game, source_id, first_ts, count) VALUES(?, ?, ?, 1) " +
                "ON CONFLICT(game, source_id) DO UPDATE SET count = count + 1",
            arrayOf<Any>(game, sourceId, timestamp)
        )
        return seenBefore
    }

    fun hasSeen(game: String, sourceId: String): Boolean = readableDatabase.rawQuery(
        "SELECT 1 FROM seen WHERE game = ? AND source_id = ?",
        arrayOf(game, sourceId)
    ).use { it.moveToFirst() }

    // ---- Activity queries ----

    /** Totals for [game], or for every game when null. */
    fun totals(game: String? = null): ReadingCounters = sumDaily(
        if (game == null) null else "game = ?",
        if (game == null) emptyArray() else arrayOf(game)
    )

    /** Totals of one reading day, for [game] or for every game when null. */
    fun dayTotals(day: String, game: String? = null): ReadingCounters = if (game == null) {
        sumDaily("day = ?", arrayOf(day))
    } else {
        sumDaily("day = ? AND game = ?", arrayOf(day, game))
    }

    /** Totals per reading day, newest first, at most [limit] days. */
    fun dailyHistory(game: String? = null, limit: Int = 30): List<Pair<String, ReadingCounters>> {
        val where = if (game == null) "" else "WHERE game = ?"
        val args = if (game == null) emptyArray() else arrayOf(game)
        val out = ArrayList<Pair<String, ReadingCounters>>()
        readableDatabase.rawQuery(
            "SELECT day, $SUM_COLUMNS FROM daily $where GROUP BY day ORDER BY day DESC LIMIT $limit",
            args
        ).use { c -> while (c.moveToNext()) out.add(c.getString(0) to countersFrom(c, 1)) }
        return out
    }

    /** Every game that has activity or a known title, with its totals, most read first. */
    fun gameTotals(): List<GameTotals> {
        val out = ArrayList<GameTotals>()
        readableDatabase.rawQuery(
            """SELECT d.game, COALESCE(g.title, d.game), $SUM_COLUMNS
               FROM daily d LEFT JOIN games g ON g.game = d.game
               GROUP BY d.game ORDER BY SUM(d.chars) DESC""",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) {
                out.add(GameTotals(c.getString(0), c.getString(1), countersFrom(c, 2)))
            }
        }
        return out
    }

    /** Sessions of [game] (or all games), newest first, each with its totals. */
    fun sessions(game: String? = null): List<SessionRecord> {
        val where = if (game == null) "" else "WHERE s.game = ?"
        val args = if (game == null) emptyArray() else arrayOf(game)
        val out = ArrayList<SessionRecord>()
        readableDatabase.rawQuery(
            """SELECT s.id, s.game, s.started, s.last_activity, s.ended,
                      COALESCE(SUM(d.active_ms), 0), COALESCE(SUM(d.chars), 0),
                      COALESCE(SUM(d.unique_chars), 0), COALESCE(SUM(d.reread_chars), 0),
                      COALESCE(SUM(d.lines), 0), COALESCE(SUM(d.choice_chars), 0)
               FROM sessions s LEFT JOIN daily d ON d.session_id = s.id
               $where GROUP BY s.id ORDER BY s.id DESC""",
            args
        ).use { c -> while (c.moveToNext()) out.add(sessionFrom(c, countersFrom(c, 5))) }
        return out
    }

    /** Removes one session and everything it contributed to the totals. */
    fun deleteSession(id: Long) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            db.delete("daily", "session_id = ?", arrayOf(id.toString()))
            db.delete("sessions", "id = ?", arrayOf(id.toString()))
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    /**
     * Removes the sessions and daily totals of [game], or of every game when null. The backlog
     * and the seen sources stay, so passed text still counts as rereading afterwards.
     */
    fun clearActivity(game: String? = null) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            if (game == null) {
                db.delete("daily", null, null)
                db.delete("sessions", null, null)
            } else {
                db.delete("daily", "game = ?", arrayOf(game))
                db.delete("sessions", "game = ?", arrayOf(game))
            }
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
    }

    // ---- Export ----

    /**
     * What has been read, by reading day and game, with the games' names: what this install
     * sends to the save sync service, whole each time ("days" and "games" of its record).
     */
    fun statsRecord(): JSONObject {
        val games = JSONObject()
        readableDatabase.rawQuery(
            "SELECT game, title FROM games WHERE game IN (SELECT DISTINCT game FROM daily)",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) games.put(c.getString(0), c.getString(1))
        }
        val days = JSONArray()
        readableDatabase.rawQuery(
            """SELECT day, game, SUM(active_ms), SUM(chars), SUM(unique_chars), SUM(reread_chars),
                      SUM(lines), SUM(choice_chars), COUNT(DISTINCT session_id)
               FROM daily GROUP BY day, game ORDER BY day, game""",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) {
                days.put(
                    JSONObject()
                        .put("day", c.getString(0))
                        .put("game", c.getString(1))
                        .put("active_ms", c.getLong(2))
                        .put("chars", c.getLong(3))
                        .put("unique_chars", c.getLong(4))
                        .put("reread_chars", c.getLong(5))
                        .put("lines", c.getLong(6))
                        .put("choice_chars", c.getLong(7))
                        .put("sessions", c.getLong(8))
                )
            }
        }
        return JSONObject().put("games", games).put("days", days)
    }

    /** Activity of every game: sessions with their per-day ledger, plus totals. */
    fun exportActivityJson(): JSONObject {
        val root = JSONObject()
        root.put("format", "azahar-reader.activity")
        root.put("version", 1)
        val titles = HashMap<String, String>()
        readableDatabase.rawQuery("SELECT game, title FROM games", emptyArray()).use { c ->
            while (c.moveToNext()) titles[c.getString(0)] = c.getString(1)
        }
        val ledger = HashMap<Long, JSONArray>()
        readableDatabase.rawQuery(
            "SELECT session_id, day, active_ms, chars, unique_chars, reread_chars, lines, " +
                "choice_chars FROM daily ORDER BY session_id, day",
            emptyArray()
        ).use { c ->
            while (c.moveToNext()) {
                ledger.getOrPut(c.getLong(0)) { JSONArray() }
                    .put(countersJson(countersFrom(c, 2)).put("day", c.getString(1)))
            }
        }
        val sessions = JSONArray()
        for (s in sessions().asReversed()) {
            sessions.put(
                countersJson(s.counters).apply {
                    put("id", s.id)
                    put("game", s.game)
                    put("title", titles[s.game] ?: s.game)
                    put("started", s.started)
                    put("lastActivity", s.lastActivity)
                    put("ended", s.ended ?: JSONObject.NULL)
                    put("days", ledger[s.id] ?: JSONArray())
                }
            )
        }
        root.put("sessions", sessions)
        val games = JSONArray()
        for (g in gameTotals()) {
            games.put(countersJson(g.counters).put("game", g.game).put("title", g.title))
        }
        root.put("games", games)
        root.put("total", countersJson(totals()))
        return root
    }

    /** One CSV row per session, oldest first. */
    fun exportSessionsCsv(): String = buildString {
        append(
            "session,game,title,started,last_activity,ended,active_seconds,characters," +
                "unique_characters,reread_characters,lines,choice_characters\n"
        )
        val titles = HashMap<String, String>()
        readableDatabase.rawQuery("SELECT game, title FROM games", emptyArray()).use { c ->
            while (c.moveToNext()) titles[c.getString(0)] = c.getString(1)
        }
        for (s in sessions().asReversed()) {
            val title = (titles[s.game] ?: s.game).replace("\"", "\"\"")
            append(s.id).append(',').append(s.game).append(",\"").append(title).append("\",")
            append(s.started).append(',').append(s.lastActivity).append(',')
            append(s.ended?.toString() ?: "").append(',')
            append(s.counters.activeMs / 1000).append(',').append(s.counters.characters).append(',')
            append(s.counters.uniqueCharacters).append(',')
            append(s.counters.rereadCharacters).append(',').append(s.counters.lines).append(',')
            append(s.counters.choiceCharacters).append('\n')
        }
    }

    private fun countersJson(c: ReadingCounters) = JSONObject().apply {
        put("activeMs", c.activeMs)
        put("characters", c.characters)
        put("uniqueCharacters", c.uniqueCharacters)
        put("rereadCharacters", c.rereadCharacters)
        put("lines", c.lines)
        put("choiceCharacters", c.choiceCharacters)
    }

    private fun sumDaily(where: String?, args: Array<String>): ReadingCounters =
        readableDatabase.rawQuery(
            "SELECT $SUM_COLUMNS FROM daily" + if (where == null) "" else " WHERE $where",
            args
        ).use { c -> if (c.moveToFirst()) countersFrom(c, 0) else ReadingCounters() }

    private fun countersFrom(c: Cursor, first: Int) = ReadingCounters(
        activeMs = c.getLong(first),
        characters = c.getLong(first + 1),
        uniqueCharacters = c.getLong(first + 2),
        rereadCharacters = c.getLong(first + 3),
        lines = c.getLong(first + 4),
        choiceCharacters = c.getLong(first + 5)
    )

    private fun sessionFrom(c: Cursor, counters: ReadingCounters) = SessionRecord(
        id = c.getLong(0),
        game = c.getString(1),
        started = c.getLong(2),
        lastActivity = c.getLong(3),
        ended = if (c.isNull(4)) null else c.getLong(4),
        counters = counters
    )

    companion object {
        const val DATABASE_NAME = "reader.db"
        private const val VERSION = 2
        private const val SUM_COLUMNS =
            "COALESCE(SUM(active_ms), 0), COALESCE(SUM(chars), 0), " +
                "COALESCE(SUM(unique_chars), 0), COALESCE(SUM(reread_chars), 0), " +
                "COALESCE(SUM(lines), 0), COALESCE(SUM(choice_chars), 0)"
    }
}
