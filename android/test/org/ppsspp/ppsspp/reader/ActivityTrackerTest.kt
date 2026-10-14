// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.stats.ActivityStore
import org.ppsspp.ppsspp.reader.stats.ActivityTracker
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.ReadingRules
import org.ppsspp.ppsspp.reader.stats.SessionRecord
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ActivityTrackerTest {
    /** In-memory store that records exactly what the tracker persists. */
    private class FakeStore : ActivityStore {
        data class Session(val id: Long, val game: String, val started: Long, var last: Long, var ended: Long?)

        val sessions = ArrayList<Session>()
        val daily = LinkedHashMap<Pair<Long, String>, ReadingCounters>()
        val seen = HashSet<String>()

        override fun openSession(game: String): SessionRecord? =
            sessions.lastOrNull { it.game == game && it.ended == null }
                ?.let { SessionRecord(it.id, it.game, it.started, it.last, it.ended) }

        override fun createSession(game: String, started: Long): Long {
            val id = sessions.size + 1L
            sessions.add(Session(id, game, started, started, null))
            return id
        }

        override fun sessionCounters(id: Long): ReadingCounters =
            daily.filterKeys { it.first == id }.values.fold(ReadingCounters()) { a, b -> a + b }

        override fun closeSession(id: Long, ended: Long) {
            sessions.first { it.id == id }.ended = ended
        }

        override fun addActivity(
            sessionId: Long,
            game: String,
            day: String,
            delta: ReadingCounters,
            lastActivity: Long
        ) {
            val key = sessionId to day
            daily[key] = (daily[key] ?: ReadingCounters()) + delta
            sessions.first { it.id == sessionId }.let { it.last = maxOf(it.last, lastActivity) }
        }

        override fun markSeen(game: String, sourceId: String, timestamp: Long): Boolean =
            !seen.add("$game/$sourceId")

        fun total() = daily.values.fold(ReadingCounters()) { a, b -> a + b }
    }

    private fun line(text: String, id: String, channel: String = "dialogue") = TextLine(
        sequence = 0, frame = 0, audioPosition = 0, channel = channel, speaker = "声", text = text,
        ruby = emptyList(), sourceId = id, meta = emptyMap(), timestamp = 0
    )

    private val day = "2026-10-01"
    private val t0 = 1_000_000_000L
    private val second = 1000L
    private val hour = 3_600_000L

    private fun tracker(store: FakeStore, dayOf: (Long) -> String = { day }) =
        ActivityTracker(store, "GAME", dayOf)

    @Test
    fun countsNarrativeAndTellsRereadingApart() {
        val store = FakeStore()
        val tracker = tracker(store)
        val first = tracker.onLine(line("道を見張れ", "a"), t0)
        assertEquals(5, first.characters)
        assertFalse(first.reread)
        assertTrue(first.counted)
        // Shown again in the same session: the same reading, counted once.
        val again = tracker.onLine(line("道を見張れ", "a"), t0 + 10 * second)
        assertTrue(again.reread)
        assertFalse(again.counted)
        // So are the same words under another source, as a game gives them after a menu.
        assertFalse(tracker.onLine(line("道を見張れ", "a2"), t0 + 11 * second).counted)
        assertFalse(tracker.onLine(line(" 道を見張れ\n", "a3"), t0 + 12 * second).counted)
        // A line of signs alone is no reading.
        assertFalse(tracker.onLine(line("……！？", "dots"), t0 + 13 * second).counted)
        tracker.flush()
        var total = store.total()
        assertEquals(5, total.characters)
        assertEquals(5, total.uniqueCharacters)
        assertEquals(0, total.rereadCharacters)
        assertEquals(1, total.lines)
        assertEquals(0, total.choiceCharacters)

        // Met again in a later session it is rereading, and counted as that.
        val later = tracker.onLine(line("道を見張れ", "a"), t0 + 5 * hour)
        assertTrue(later.reread)
        assertTrue(later.counted)
        tracker.flush()
        total = store.total()
        assertEquals(10, total.characters)
        assertEquals(5, total.uniqueCharacters)
        assertEquals(5, total.rereadCharacters)
        assertEquals(2, total.lines)
    }

    @Test
    fun reportsWhatTheOpenSessionHoldsSavedOrNot() {
        val store = FakeStore()
        val tracker = tracker(store)
        assertNull("nothing was read yet", tracker.session)
        tracker.onLine(line("道を見張れ", "a"), t0)
        tracker.onHeartbeat(t0 + 2 * second, reading = true)
        assertEquals(5, tracker.session!!.characters)
        assertEquals(2 * second, tracker.session!!.activeMs)
        tracker.flush()
        tracker.onLine(line("旅人だ", "b"), t0 + 3 * second)
        assertEquals("saving does not count anything twice", 8, tracker.session!!.characters)
        tracker.flush()

        // The app is restarted within the session: it goes on from what it had.
        val later = tracker(store)
        assertNull(later.session)
        later.onLine(line("君は", "c"), t0 + hour)
        assertEquals(10, later.session!!.characters)
        assertEquals(3, later.session!!.lines)
        assertEquals(2 * second, later.session!!.activeMs)

        // After a long break a new session starts from nothing.
        later.onLine(line("まだない", "d"), t0 + hour + ReadingRules.SESSION_GAP_MS + second)
        assertEquals(4, later.session!!.characters)
        later.reset()
        assertNull(later.session)
    }

    @Test
    fun choicesAreCountedSeparately() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.onLine(line("はい\nいいえ", "c", channel = "choice"), t0)
        tracker.flush()
        val total = store.total()
        assertEquals(0, total.characters)
        assertEquals(0, total.lines)
        assertEquals(5, total.choiceCharacters)
    }

    @Test
    fun activeTimeNeedsNarrativeAndIsCapped() {
        val store = FakeStore()
        val tracker = tracker(store)
        // No line yet: nothing accrues.
        tracker.onHeartbeat(t0, reading = true)
        tracker.onHeartbeat(t0 + second, reading = true)
        tracker.onLine(line("あいう", "a"), t0 + second)
        tracker.onHeartbeat(t0 + 2 * second, reading = true)
        tracker.onHeartbeat(t0 + 3 * second, reading = true)
        // A long gap between heartbeats credits five seconds at most.
        tracker.onHeartbeat(t0 + 63 * second, reading = true)
        // Not reading (paused or in the background): nothing.
        tracker.onHeartbeat(t0 + 64 * second, reading = false)
        tracker.onHeartbeat(t0 + 65 * second, reading = true)
        tracker.flush()
        assertEquals(2 * second + ReadingRules.MAX_HEARTBEAT_MS + second, store.total().activeMs)
    }

    @Test
    fun idleTimeoutStopsTheClockUntilReadingResumes() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.onLine(line("あ", "a"), t0)
        var now = t0
        // Six minutes of heartbeats with no new line: only the first five count.
        repeat(360) {
            now += second
            tracker.onHeartbeat(now, reading = true)
        }
        tracker.flush()
        assertEquals(300 * second, store.total().activeMs)
        // A lookup after the timeout does not revive the clock; a new line does.
        tracker.onInteraction(now)
        tracker.onHeartbeat(now + second, reading = true)
        tracker.onLine(line("い", "b"), now + 2 * second)
        tracker.onHeartbeat(now + 3 * second, reading = true)
        tracker.flush()
        assertEquals(301 * second, store.total().activeMs)
    }

    @Test
    fun interactionKeepsTheClockRunning() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.onLine(line("あ", "a"), t0)
        var now = t0
        repeat(400) { i ->
            now += second
            if (i == 200) tracker.onInteraction(now) // a dictionary lookup at 3:20
            tracker.onHeartbeat(now, reading = true)
        }
        tracker.flush()
        assertEquals(400 * second, store.total().activeMs)
    }

    @Test
    fun pausedTrackerCountsNothing() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.paused = true
        val accounting = tracker.onLine(line("あいう", "a"), t0)
        assertFalse(accounting.counted)
        assertNull(accounting.sessionId)
        tracker.onHeartbeat(t0 + second, reading = true)
        tracker.flush()
        assertTrue(store.sessions.isEmpty())
        assertTrue(store.daily.isEmpty())
    }

    @Test
    fun shortBreakContinuesTheSessionLongBreakStartsANewOne() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.onLine(line("あ", "a"), t0)
        tracker.onLine(line("い", "b"), t0 + 3 * hour)
        assertEquals(1, store.sessions.size)
        tracker.onLine(line("う", "c"), t0 + 3 * hour + 4 * hour)
        tracker.flush()
        assertEquals(2, store.sessions.size)
        // The first session closed at its last activity, not when the next one began.
        assertEquals(t0 + 3 * hour, store.sessions[0].ended)
        assertNull(store.sessions[1].ended)
        assertEquals(2, store.daily[1L to day]!!.characters)
        assertEquals(1, store.daily[2L to day]!!.characters)
    }

    @Test
    fun aNewTrackerResumesAnOpenSessionWithinFourHours() {
        val store = FakeStore()
        tracker(store).apply {
            onLine(line("あ", "a"), t0)
            flush()
        }
        // The app was restarted an hour later.
        tracker(store).apply {
            onLine(line("い", "b"), t0 + hour)
            flush()
        }
        assertEquals(1, store.sessions.size)
        // And again five hours after that: a new session, the old one closed.
        tracker(store).apply {
            onLine(line("う", "c"), t0 + 6 * hour)
            flush()
        }
        assertEquals(2, store.sessions.size)
        assertEquals(t0 + hour, store.sessions[0].ended)
    }

    @Test
    fun aSessionAcrossTheDayBoundaryKeepsAPerDayLedger() {
        val store = FakeStore()
        val boundary = t0 + hour
        val tracker = tracker(store) { if (it < boundary) "2026-10-01" else "2026-10-02" }
        tracker.onLine(line("あいう", "a"), t0)
        tracker.onLine(line("えお", "b"), boundary + second)
        tracker.flush()
        assertEquals(1, store.sessions.size)
        assertEquals(3, store.daily[1L to "2026-10-01"]!!.characters)
        assertEquals(2, store.daily[1L to "2026-10-02"]!!.characters)
    }

    @Test
    fun resetForgetsTheOpenSession() {
        val store = FakeStore()
        val tracker = tracker(store)
        tracker.onLine(line("あ", "a"), t0)
        tracker.flush()
        // The user deleted the session in the statistics screen.
        store.sessions.clear()
        store.daily.clear()
        tracker.reset()
        tracker.onLine(line("い", "b"), t0 + second)
        tracker.flush()
        assertEquals(1, store.sessions.size)
        assertEquals(1, store.total().characters)
    }
}
