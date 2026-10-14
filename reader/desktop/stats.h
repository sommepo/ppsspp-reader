// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <QStringList>
#include <QSet>
#include <QList>
#include <QString>
#include "reader/desktop/text_line.h"

namespace ReaderApp {

/// Reading counters for one bucket: a session's share of a day, a session, a day or a game.
struct ReadingCounters {
    qint64 active_ms{};
    qint64 characters{};
    qint64 unique_characters{};
    qint64 reread_characters{};
    qint64 lines{};
    qint64 choice_characters{};

    bool operator==(const ReadingCounters&) const = default;

    [[nodiscard]] bool IsEmpty() const {
        return *this == ReadingCounters{};
    }
    [[nodiscard]] ReadingCounters operator+(const ReadingCounters& other) const {
        return ReadingCounters{active_ms + other.active_ms,
                               characters + other.characters,
                               unique_characters + other.unique_characters,
                               reread_characters + other.reread_characters,
                               lines + other.lines,
                               choice_characters + other.choice_characters};
    }
    [[nodiscard]] std::optional<double> CharactersPerHour() const;
};

/// A reading session of one game. `ended` is empty while the session can still be resumed.
struct SessionRecord {
    qint64 id{};
    QString game;
    qint64 started{};
    qint64 last_activity{};
    std::optional<qint64> ended;
    ReadingCounters counters;
};

/// How one presented line was counted.
struct LineAccounting {
    int characters{};
    bool reread{};
    bool counted{};
    std::optional<qint64> session_id;
};

/// Persistence used by ActivityTracker. Implemented by the reader database.
class ActivityStore {
public:
    virtual ~ActivityStore() = default;

    /// The most recent session of `game` that has not been closed, or nothing.
    virtual std::optional<SessionRecord> OpenSession(const QString& game) = 0;
    virtual qint64 CreateSession(const QString& game, qint64 started) = 0;
    /// Everything saved for session `id` so far.
    virtual ReadingCounters SessionCounters(qint64 id) = 0;
    virtual void CloseSession(qint64 id, qint64 ended) = 0;
    /// Adds `delta` to the session's row for `day` and moves its last activity forward.
    virtual void AddActivity(qint64 session_id, const QString& game, const QString& day,
                             const ReadingCounters& delta, qint64 last_activity) = 0;
    /// Records a presentation of `source_id`; true when it had been presented before.
    virtual bool MarkSeen(const QString& game, const QString& source_id, qint64 timestamp) = 0;
    /// The sources already counted in session `id`, for a session that is continued after
    /// the app was closed: a line of them shown again is not counted twice. Empty where that
    /// is not known.
    virtual QStringList SessionSources(qint64) {
        return {};
    }
};

/// Counting rules shared with VN Library, so numbers from both readers mean the same thing.
namespace ReadingRules {
/// A reading day starts at 04:01 local time; 04:00:59 still belongs to the previous day.
inline constexpr int DayStartMinutes = 4 * 60 + 1;
/// A break shorter than this continues the previous session.
inline constexpr qint64 SessionGapMs = 4 * 60 * 60 * 1000LL;
/// Active time stops accruing after this long without reading activity.
inline constexpr qint64 DefaultIdleTimeoutMs = 5 * 60 * 1000LL;
/// A single heartbeat never credits more than this.
inline constexpr qint64 MaxHeartbeatMs = 5000;

/**
 * Characters of narrative text: the Japanese ones only, kana and kanji. Punctuation, symbols,
 * digits, Latin letters and whitespace are not reading and do not count. Speaker names and
 * ruby readings are separate fields and never reach this function.
 */
[[nodiscard]] int CountCharacters(const QString& text);
/// What of `text` was not shown before: `text` without the stretches of it that stand in one
/// of the lines shown just before it (`before`). A game that shows a long text a screenful
/// at a time, scrolled by the reader, shows most of every screenful twice: only what is new
/// was read. A stretch counts as shown before from FreshOverlap characters on, and only the
/// last FreshWindow lines are looked at.
inline constexpr qsizetype FreshOverlap = 12;
inline constexpr qsizetype FreshWindow = 12;
[[nodiscard]] QString WithoutOverlap(const QString& text, const QStringList& before);
/// Kana (with the long vowel mark and the iteration marks; half width too) and kanji (with
/// 々, 〆 and 〇). Not the middle dot, brackets or any other sign.
[[nodiscard]] bool IsJapanese(char32_t cp);
/// The reading day (ISO date) that `epoch_ms` belongs to, in local time.
[[nodiscard]] QString ReadingDay(qint64 epoch_ms);
/// Characters per hour, or nothing below one second of active time.
[[nodiscard]] std::optional<double> CharactersPerHour(qint64 characters, qint64 active_ms);
} // namespace ReadingRules

/**
 * Turns presented lines and elapsed time into reading activity for one game.
 *
 * Rules (shared with VN Library):
 *  - narrative characters exclude whitespace; choices are counted separately;
 *  - a source presented again is rereading, not unique text;
 *  - active time accrues only while reading is visible, not manually paused and within the
 *    idle timeout of the last reading activity; one heartbeat credits at most five seconds;
 *  - a break shorter than four hours continues the session, a longer one closes it at its
 *    last activity;
 *  - days begin at 04:01 local time.
 *
 * Not thread-safe: call from one thread.
 */
class ActivityTracker {
public:
    using DayOf = std::function<QString(qint64)>;

    ActivityTracker(ActivityStore& store_, QString game_, DayOf day_of_ = {},
                    qint64 idle_timeout_ms_ = ReadingRules::DefaultIdleTimeoutMs);

    /// While true nothing is counted: neither characters nor time.
    bool paused{};
    qint64 idle_timeout_ms;

    /// The session currently being added to, if any.
    [[nodiscard]] std::optional<qint64> CurrentSessionId() const {
        return session_id;
    }
    /**
     * What the session being added to holds, saved or not; nothing before the first counted
     * line. A session continued after a restart starts from what it had.
     */
    [[nodiscard]] std::optional<ReadingCounters> Session() const {
        return session;
    }

    /// Counts a newly presented line. Call once per presentation, never for restored text.
    LineAccounting OnLine(const TextLine& line, qint64 now);
    /// The user did something that shows they are still reading (lookup, advance, scroll).
    void OnInteraction(qint64 now);
    /**
     * Credits active time up to `now`. `reading` is false while emulation is paused, hidden
     * or a non-reading screen (settings, backlog) is in front.
     */
    void OnHeartbeat(qint64 now, bool reading);
    /// Writes unsaved contributions. Call regularly and whenever reading stops.
    void Flush();
    /// Forgets the open session, for example after the user deleted or cleared it.
    void Reset();

private:
    void Add(qint64 now, const ReadingCounters& delta);
    qint64 EnsureSession(qint64 now);

    ActivityStore& store;
    QString game;
    DayOf day_of;
    std::optional<qint64> session_id;
    std::optional<ReadingCounters> session;
    qint64 last_activity{}; ///< Last reading activity credited to the session
    qint64 last_heartbeat{};
    bool has_narrative{};
    /// True when `line` was counted in this session already; notes it when it was not.
    bool IsRepeat(const TextLine& line);
    /// What this session has counted: its sources, and the text of its last lines (a game
    /// may give the same words another source when it shows them again).
    static constexpr qsizetype RecentTexts = 40;
    QSet<QString> counted_sources;
    QStringList recent_texts;
    /// Unsaved contributions per reading day, in the order the days were met.
    QList<std::pair<QString, ReadingCounters>> pending;
};

/// A reading session in a few characters, to sit in a corner of the game:
/// "1,234字 · 5,430/h · 12m".
namespace SessionSummary {
/// A speed worked out from less reading than this jumps about too much to show.
inline constexpr qint64 SpeedAfterMs = 60000;

[[nodiscard]] QString Format(const ReadingCounters& counters, bool paused = false);
/// "<1m", "12m" or "1h05m": minutes, so that it does not tick.
[[nodiscard]] QString Duration(qint64 active_ms);
} // namespace SessionSummary

} // namespace ReaderApp
