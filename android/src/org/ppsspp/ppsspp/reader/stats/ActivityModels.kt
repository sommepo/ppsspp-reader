// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.stats

/** Reading counters for one bucket: a session's share of a day, a session, a day or a game. */
data class ReadingCounters(
    val activeMs: Long = 0,
    val characters: Long = 0,
    val uniqueCharacters: Long = 0,
    val rereadCharacters: Long = 0,
    val lines: Long = 0,
    val choiceCharacters: Long = 0
) {
    val isEmpty: Boolean
        get() = activeMs == 0L && characters == 0L && uniqueCharacters == 0L &&
            rereadCharacters == 0L && lines == 0L && choiceCharacters == 0L

    operator fun plus(other: ReadingCounters) = ReadingCounters(
        activeMs + other.activeMs,
        characters + other.characters,
        uniqueCharacters + other.uniqueCharacters,
        rereadCharacters + other.rereadCharacters,
        lines + other.lines,
        choiceCharacters + other.choiceCharacters
    )

    val charactersPerHour: Double?
        get() = ReadingRules.charactersPerHour(characters, activeMs)
}

/** A reading session of one game. [ended] is null while the session can still be resumed. */
data class SessionRecord(
    val id: Long,
    val game: String,
    val started: Long,
    val lastActivity: Long,
    val ended: Long?,
    val counters: ReadingCounters = ReadingCounters()
)

/** How one presented line was counted. */
data class LineAccounting(
    val characters: Int,
    val reread: Boolean,
    val counted: Boolean,
    val sessionId: Long?
)

/** Persistence used by [ActivityTracker]. Implemented by the reader database. */
interface ActivityStore {
    /** The most recent session of [game] that has not been closed, or null. */
    fun openSession(game: String): SessionRecord?

    fun createSession(game: String, started: Long): Long

    /** Everything saved for session [id] so far. */
    fun sessionCounters(id: Long): ReadingCounters

    fun closeSession(id: Long, ended: Long)

    /** Adds [delta] to the session's row for [day] and moves its last activity forward. */
    fun addActivity(
        sessionId: Long,
        game: String,
        day: String,
        delta: ReadingCounters,
        lastActivity: Long
    )

    /** Records a presentation of [sourceId]; returns true when it had been presented before. */
    fun markSeen(game: String, sourceId: String, timestamp: Long): Boolean

    /**
     * The sources already counted in session [id], for a session that is continued after the
     * app was closed: a line of them shown again is not counted twice. Empty where that is
     * not known.
     */
    fun sessionSources(id: Long): Collection<String> = emptyList()
}
