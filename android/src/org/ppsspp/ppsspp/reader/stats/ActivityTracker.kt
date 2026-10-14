// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.stats

import org.ppsspp.ppsspp.reader.model.TextLine

/**
 * Turns presented lines and elapsed time into reading activity for one game.
 *
 * Rules (shared with VN Library):
 *  - narrative characters exclude whitespace; choices are counted separately;
 *  - only Japanese characters count (kana and kanji), not punctuation, symbols or Latin text;
 *  - a line shown again within the same session (the same source, or the same text as one of
 *    the last lines) is the same reading and is not counted again;
 *  - a source presented again in a later session is rereading, not unique text;
 *  - active time accrues only while reading is visible, not manually paused and within the idle
 *    timeout of the last reading activity; one heartbeat credits at most five seconds;
 *  - a break shorter than four hours continues the session, a longer one closes it at its last
 *    activity;
 *  - days begin at 04:01 local time.
 *
 * Not thread-safe: call from one thread.
 */
class ActivityTracker(
    private val store: ActivityStore,
    private val game: String,
    private val dayOf: (Long) -> String = { ReadingRules.readingDay(it) },
    var idleTimeoutMs: Long = ReadingRules.DEFAULT_IDLE_TIMEOUT_MS
) {
    private companion object {
        /** How many of the last counted texts a new line is compared with. */
        const val RECENT_TEXTS = 40
    }

    /** While true nothing is counted: neither characters nor time. */
    var paused: Boolean = false

    private var sessionId: Long? = null
    private var lastActivity: Long = 0 // last reading activity credited to the session
    private var lastHeartbeat: Long = 0
    private var hasNarrative = false

    // What this session has counted: its sources, and the text of its last lines (a game may
    // give the same words another source when it shows them again).
    private val countedSources = HashSet<String>()
    private val recentTexts = ArrayDeque<String>()

    // Unsaved contributions, keyed by reading day.
    private val pending = LinkedHashMap<String, ReadingCounters>()

    /** The session currently being added to, if any. */
    val currentSessionId: Long? get() = sessionId

    /**
     * What the session being added to holds, saved or not; null before the first counted line.
     * A session continued after a restart starts from what it had.
     */
    var session: ReadingCounters? = null
        private set

    /** Counts a newly presented line. Call once per presentation, never for restored text. */
    fun onLine(line: TextLine, now: Long): LineAccounting {
        val characters = ReadingRules.countCharacters(line.text)
        if (paused || characters == 0) {
            return LineAccounting(characters, reread = false, counted = false, sessionId = null)
        }
        // Coming back from a pause in reading: time before this line is not reading time.
        val resuming = sessionId == null || now - lastActivity > idleTimeoutMs
        val id = ensureSession(now)
        if (isRepeat(line)) {
            // Shown again (a menu closed over it, a save loaded, the game repeating itself):
            // the reader is still there, and nothing new was read.
            lastActivity = now
            if (resuming || lastHeartbeat == 0L) lastHeartbeat = now
            return LineAccounting(characters, reread = true, counted = false, sessionId = null)
        }
        val reread = store.markSeen(game, line.sourceId, now)
        val delta = if (line.isChoice) {
            ReadingCounters(choiceCharacters = characters.toLong())
        } else {
            hasNarrative = true
            ReadingCounters(
                characters = characters.toLong(),
                uniqueCharacters = if (reread) 0 else characters.toLong(),
                rereadCharacters = if (reread) characters.toLong() else 0,
                lines = 1
            )
        }
        add(now, delta)
        lastActivity = now
        if (resuming || lastHeartbeat == 0L) lastHeartbeat = now
        return LineAccounting(characters, reread, counted = true, sessionId = id)
    }

    /** True when [line] was counted in this session already; notes it when it was not. */
    private fun isRepeat(line: TextLine): Boolean {
        val text = line.text.trim()
        val known = (line.sourceId.isNotEmpty() && line.sourceId in countedSources) || text in recentTexts
        if (line.sourceId.isNotEmpty()) countedSources.add(line.sourceId)
        if (!known) {
            recentTexts.addLast(text)
            if (recentTexts.size > RECENT_TEXTS) recentTexts.removeFirst()
        }
        return known
    }

    /** The user did something that shows they are still reading (lookup, advance, scroll). */
    fun onInteraction(now: Long) {
        if (paused || sessionId == null) return
        if (now - lastActivity <= idleTimeoutMs) lastActivity = now
    }

    /**
     * Credits active time up to [now]. [reading] is false while emulation is paused, hidden or
     * a non-reading screen (settings, backlog) is in front.
     */
    fun onHeartbeat(now: Long, reading: Boolean) {
        val previous = lastHeartbeat
        lastHeartbeat = now
        if (paused || !reading || !hasNarrative || sessionId == null || previous == 0L) return
        if (now <= previous || now - lastActivity > idleTimeoutMs) return
        if (now - lastActivity > ReadingRules.SESSION_GAP_MS) return
        val credit = minOf(now - previous, ReadingRules.MAX_HEARTBEAT_MS)
        add(now, ReadingCounters(activeMs = credit))
    }

    /** Writes unsaved contributions. Call regularly and whenever reading stops. */
    fun flush() {
        val id = sessionId ?: return
        if (pending.isEmpty()) return
        for ((day, delta) in pending) {
            store.addActivity(id, game, day, delta, lastActivity)
        }
        pending.clear()
    }

    /** Forgets the open session, for example after the user deleted or cleared it. */
    fun reset() {
        pending.clear()
        countedSources.clear()
        recentTexts.clear()
        sessionId = null
        session = null
        hasNarrative = false
        lastActivity = 0
        lastHeartbeat = 0
    }

    private fun add(now: Long, delta: ReadingCounters) {
        val day = dayOf(now)
        pending[day] = (pending[day] ?: ReadingCounters()) + delta
        session = (session ?: ReadingCounters()) + delta
    }

    private fun ensureSession(now: Long): Long {
        sessionId?.let { id ->
            if (now - lastActivity < ReadingRules.SESSION_GAP_MS) return id
            flush()
            store.closeSession(id, lastActivity)
            sessionId = null
        }
        val open = store.openSession(game)
        val id = if (open != null && now - open.lastActivity < ReadingRules.SESSION_GAP_MS) {
            open.id
        } else {
            if (open != null) store.closeSession(open.id, open.lastActivity)
            store.createSession(game, now)
        }
        sessionId = id
        countedSources.clear()
        countedSources.addAll(store.sessionSources(id))
        recentTexts.clear()
        session = store.sessionCounters(id)
        lastActivity = now
        return id
    }
}
