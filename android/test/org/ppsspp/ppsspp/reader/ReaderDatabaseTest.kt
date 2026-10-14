// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.stats.ActivityTracker
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.store.ReaderDatabase
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class ReaderDatabaseTest {
    private lateinit var db: ReaderDatabase
    private val game = "0004000000088A00"
    private val other = "0004000000055D00"

    @Before
    fun setUp() {
        // A null name gives an in-memory database: nothing outlives the test.
        db = ReaderDatabase(ApplicationProvider.getApplicationContext<Context>(), null)
    }

    @After
    fun tearDown() {
        db.close()
    }

    private fun line(text: String, id: String, ts: Long, speaker: String = "", channel: String = "dialogue") =
        TextLine(
            sequence = 0, frame = 0, audioPosition = -1, channel = channel, speaker = speaker,
            text = text, ruby = listOf(RubySpan(0, 1, "よみ")), sourceId = id,
            meta = mapOf("voice" to "2004"), timestamp = ts
        )

    @Test
    fun backlogKeepsLinesNewestFirstAndPages() {
        val ids = (1..5).map { i ->
            db.insertLine(game, line("行$i", "s$i", 1000L + i, speaker = "声"), LineAccounting(2, i == 5, true, null))
        }
        db.insertLine(other, line("別のゲーム", "x", 2000L), LineAccounting(5, false, true, null))

        val recent = db.recentLines(game, 3)
        assertEquals(listOf("行5", "行4", "行3"), recent.map { it.line.text })
        assertTrue(recent[0].reread)
        assertEquals("声", recent[0].line.speaker)
        assertEquals(listOf(RubySpan(0, 1, "よみ")), recent[0].line.ruby)
        assertEquals("2004", recent[0].line.meta["voice"])
        assertEquals(1005L, recent[0].line.timestamp)

        val older = db.recentLines(game, 10, beforeId = recent.last().id)
        assertEquals(listOf("行2", "行1"), older.map { it.line.text })
        assertEquals(ids[0], older.last().id)
        assertEquals(5, db.lineCount(game))
        assertEquals(1, db.lineCount(other))
    }

    @Test
    fun searchMatchesTextAndSpeakerLiterally() {
        db.insertLine(game, line("私の名前は　まだない…", "a", 1, speaker = "少年"), LineAccounting(9, false, true, null))
        db.insertLine(game, line("100%だ", "b", 2, speaker = "タロウ"), LineAccounting(5, false, true, null))
        db.insertLine(game, line("a_b", "c", 3), LineAccounting(3, false, true, null))
        assertEquals(listOf("私の名前は　まだない…"), db.searchLines(game, "名前", 10).map { it.line.text })
        assertEquals(listOf("100%だ"), db.searchLines(game, "タロウ", 10).map { it.line.text })
        // % and _ are ordinary characters in a search, not wildcards.
        assertEquals(listOf("100%だ"), db.searchLines(game, "%", 10).map { it.line.text })
        assertEquals(listOf("a_b"), db.searchLines(game, "_", 10).map { it.line.text })
        assertTrue(db.searchLines(game, "ない言葉", 10).isEmpty())
    }

    @Test
    fun seenSourcesArePerGame() {
        assertFalse(db.markSeen(game, "s1", 10))
        assertTrue(db.markSeen(game, "s1", 20))
        assertFalse(db.markSeen(other, "s1", 30))
        assertTrue(db.hasSeen(game, "s1"))
        assertFalse(db.hasSeen(game, "s2"))
    }

    @Test
    fun theRecordSentToTheSaveSyncServiceHasEveryReadingDayOfEveryGame() {
        assertEquals(0, db.statsRecord().getJSONArray("days").length())
        val s1 = db.createSession(game, 1000)
        val s2 = db.createSession(game, 5000)
        val s3 = db.createSession(other, 2000)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 80, 20, 5, 7), 1500)
        db.addActivity(s2, game, "2026-10-01", ReadingCounters(30_000, 50, 50, 0, 2, 0), 5800)
        db.addActivity(s2, game, "2026-10-02", ReadingCounters(10_000, 10, 10, 0, 1, 0), 5900)
        db.addActivity(s3, other, "2026-10-02", ReadingCounters(5_000, 4, 4, 0, 1, 0), 2100)
        db.setGameTitle(game, "真・女神転生IV")
        db.setGameTitle("ULJM09998", "a game nothing was read of")

        val record = db.statsRecord()
        val days = record.getJSONArray("days")
        assertEquals(3, days.length())
        // By day, then by game; two sessions of a day are one entry that says so.
        val first = days.getJSONObject(0)
        assertEquals("2026-10-01", first.getString("day"))
        assertEquals(game, first.getString("game"))
        assertEquals(90_000L, first.getLong("active_ms"))
        assertEquals(150L, first.getLong("chars"))
        assertEquals(130L, first.getLong("unique_chars"))
        assertEquals(20L, first.getLong("reread_chars"))
        assertEquals(7L, first.getLong("lines"))
        assertEquals(7L, first.getLong("choice_chars"))
        assertEquals(2L, first.getLong("sessions"))
        assertEquals(listOf("2026-10-02", "2026-10-02"), (1..2).map { days.getJSONObject(it).getString("day") })
        assertEquals(setOf(game, other), (1..2).map { days.getJSONObject(it).getString("game") }.toSet())
        // The names of the games that were read, and of no other.
        val games = record.getJSONObject("games")
        assertEquals("真・女神転生IV", games.getString(game))
        assertEquals(1, games.length())

        // Cleared here, it is gone from the next record: the record is the whole of it.
        db.clearActivity(game)
        assertEquals(1, db.statsRecord().getJSONArray("days").length())
    }

    @Test
    fun whatWasCountedByTheOlderRuleIsCountedAgain() {
        val ts = 1_790_000_000_000L
        val day = org.ppsspp.ppsspp.reader.stats.ReadingRules.readingDay(ts)
        val session = db.createSession(game, ts)
        // As the older rule counted them: every character that is not a space.
        db.insertLine(game, line("…君は旅人だ？", "a", ts), LineAccounting(7, false, true, session))
        db.insertLine(game, line("……！", "b", ts), LineAccounting(3, false, true, session))
        db.insertLine(game, line("道を、見張れ。", "c", ts), LineAccounting(7, true, true, session))
        db.insertLine(game, line("はい！", "d", ts, channel = "choice"), LineAccounting(3, false, true, session))
        db.addActivity(session, game, day, ReadingCounters(60_000, 17, 10, 7, 3, 3), ts)

        db.recountCharacters(db.writableDatabase)
        // 君は旅人だ 5, nothing, 道を見張れ 5 (reread), はい 2 (a choice); the line of signs is gone.
        assertEquals(ReadingCounters(60_000, 10, 5, 5, 2, 2), db.totals(game))
        // A second time changes nothing.
        db.recountCharacters(db.writableDatabase)
        assertEquals(ReadingCounters(60_000, 10, 5, 5, 2, 2), db.totals(game))
    }

    @Test
    fun activityAddsUpPerSessionDayAndGame() {
        val s1 = db.createSession(game, 1000)
        val s2 = db.createSession(other, 2000)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 80, 20, 5, 7), 1500)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(30_000, 50, 50, 0, 2, 0), 1800)
        db.addActivity(s1, game, "2026-10-02", ReadingCounters(10_000, 10, 10, 0, 1, 0), 1900)
        db.addActivity(s2, other, "2026-10-02", ReadingCounters(5_000, 4, 4, 0, 1, 0), 2100)

        assertEquals(ReadingCounters(100_000, 160, 140, 20, 8, 7), db.totals(game))
        assertEquals(ReadingCounters(105_000, 164, 144, 20, 9, 7), db.totals())
        assertEquals(ReadingCounters(90_000, 150, 130, 20, 7, 7), db.dayTotals("2026-10-01", game))
        assertEquals(ReadingCounters(15_000, 14, 14, 0, 2, 0), db.dayTotals("2026-10-02"))
        assertEquals(listOf("2026-10-02", "2026-10-01"), db.dailyHistory().map { it.first })
        assertEquals(listOf("2026-10-02", "2026-10-01"), db.dailyHistory(game).map { it.first })

        val sessions = db.sessions(game)
        assertEquals(1, sessions.size)
        assertEquals(1900L, sessions[0].lastActivity)
        assertEquals(160L, sessions[0].counters.characters)
        assertNull(sessions[0].ended)
        assertEquals(2, db.sessions().size)

        db.setGameTitle(game, "真・女神転生IV")
        val games = db.gameTotals()
        assertEquals(listOf(game, other), games.map { it.game })
        assertEquals("真・女神転生IV", games[0].title)
        assertEquals(other, games[1].title) // no title known: the id stands in
    }

    @Test
    fun deletingASessionSubtractsExactlyItsShare() {
        val s1 = db.createSession(game, 1000)
        val s2 = db.createSession(game, 9000)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 100, 0, 5, 0), 1500)
        db.addActivity(s2, game, "2026-10-01", ReadingCounters(20_000, 30, 0, 30, 2, 0), 9500)
        db.markSeen(game, "kept", 1)
        db.insertLine(game, line("残る", "kept", 1), LineAccounting(2, false, true, s1))

        db.deleteSession(s1)
        assertEquals(ReadingCounters(20_000, 30, 0, 30, 2, 0), db.totals(game))
        assertEquals(listOf(s2), db.sessions(game).map { it.id })
        // Reading history is not activity: the backlog and seen sources stay.
        assertEquals(1, db.lineCount(game))
        assertTrue(db.hasSeen(game, "kept"))
    }

    @Test
    fun clearingActivityKeepsBacklogAndSeenSources() {
        val s1 = db.createSession(game, 1000)
        val s2 = db.createSession(other, 2000)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 100, 0, 5, 0), 1500)
        db.addActivity(s2, other, "2026-10-01", ReadingCounters(1_000, 1, 1, 0, 1, 0), 2500)
        db.markSeen(game, "a", 1)
        db.insertLine(game, line("あ", "a", 1), LineAccounting(1, false, true, s1))

        db.clearActivity(game)
        assertTrue(db.totals(game).isEmpty)
        assertTrue(db.sessions(game).isEmpty())
        assertEquals(1L, db.totals(other).characters)
        assertEquals(1, db.lineCount(game))
        assertTrue(db.hasSeen(game, "a"))

        db.clearActivity()
        assertTrue(db.totals().isEmpty)
        assertTrue(db.sessions().isEmpty())
    }

    @Test
    fun sessionsOpenCloseAndResume() {
        assertNull(db.openSession(game))
        val id = db.createSession(game, 1000)
        assertEquals(id, db.openSession(game)!!.id)
        db.closeSession(id, 5000)
        assertNull(db.openSession(game))
        assertEquals(5000L, db.sessions(game)[0].ended)
        // Closing twice keeps the first end time.
        db.closeSession(id, 9000)
        assertEquals(5000L, db.sessions(game)[0].ended)
    }

    @Test
    fun trackerAndDatabaseAgreeEndToEnd() {
        val tracker = ActivityTracker(db, game, dayOf = { "2026-10-01" })
        val t0 = 1_000_000L
        val first = line("…君は旅人だ？\n道を見張れ", "a", t0)
        val a1 = tracker.onLine(first, t0)
        db.insertLine(game, first, a1)
        for (i in 1..10) tracker.onHeartbeat(t0 + i * 1000L, reading = true)
        val again = tracker.onLine(first, t0 + 11_000)
        assertTrue(again.reread)
        assertFalse("shown again in the session: counted once", again.counted)
        tracker.flush()
        // The app is closed and opened within the session: the database knows what the
        // session counted, and the line is still not counted twice.
        val restarted = ActivityTracker(db, game, dayOf = { "2026-10-01" })
        assertFalse(restarted.onLine(first, t0 + 12_000).counted)
        restarted.flush()

        val totals = db.totals(game)
        assertEquals(10, totals.characters)
        assertEquals(10, totals.uniqueCharacters)
        assertEquals(0, totals.rereadCharacters)
        assertEquals(1, totals.lines)
        assertEquals(10_000, totals.activeMs)
        assertNotNull(db.openSession(game))
        assertEquals(a1.sessionId, db.openSession(game)!!.id)
        assertEquals("…君は旅人だ？\n道を見張れ", db.recentLines(game, 1)[0].line.text)
    }

    @Test
    fun exportsActivityAsJsonAndCsv() {
        val s1 = db.createSession(game, 1000)
        db.setGameTitle(game, "真・女神転生IV \"JP\"")
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 80, 20, 5, 7), 1500)
        db.addActivity(s1, game, "2026-10-02", ReadingCounters(1_000, 3, 3, 0, 1, 0), 1900)
        db.closeSession(s1, 1900)

        val json = db.exportActivityJson()
        assertEquals("azahar-reader.activity", json.getString("format"))
        val session = json.getJSONArray("sessions").getJSONObject(0)
        assertEquals(103, session.getInt("characters"))
        assertEquals(1900, session.getInt("ended"))
        assertEquals(2, session.getJSONArray("days").length())
        assertEquals("2026-10-01", session.getJSONArray("days").getJSONObject(0).getString("day"))
        assertEquals(103, json.getJSONObject("total").getInt("characters"))
        assertEquals(61_000, json.getJSONArray("games").getJSONObject(0).getInt("activeMs"))

        val csv = db.exportSessionsCsv().trim().lines()
        assertEquals(2, csv.size)
        assertTrue(csv[0].startsWith("session,game,title,started"))
        assertEquals("$s1,$game,\"真・女神転生IV \"\"JP\"\"\",1000,1900,1900,61,103,83,20,6,7", csv[1])
    }
}
