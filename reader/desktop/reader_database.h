// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <utility>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>
#include "reader/desktop/sqlite_db.h"
#include "reader/desktop/stats.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp {

/// A line kept in the backlog.
struct BacklogEntry {
    qint64 id{};
    QString game;
    TextLine line;
    int characters{};
    bool reread{};
};

/// Totals of one game, for the statistics screen.
struct GameTotals {
    QString game;
    QString title;
    ReadingCounters counters;
};

/**
 * Reading history kept on this machine: the backlog of presented lines, which sources were
 * seen, and reading activity per session and reading day. The file has the Android app's
 * tables.
 *
 * Activity is stored as one row per session and day, so a session, a day, a game or
 * everything can be totalled, and deleting a session subtracts exactly what it contributed.
 * Clearing activity never touches the backlog or the seen sources: text met again is still
 * rereading.
 */
class ReaderDatabase : public ActivityStore {
public:
    static constexpr const char* FileName = "reader.db";

    /// Opens (and creates) the database file, or ":memory:".
    explicit ReaderDatabase(const QString& path);

    // ---- Games ----
    void SetGameTitle(const QString& game, const QString& title);

    // ---- Backlog ----
    qint64 InsertLine(const QString& game, const TextLine& line, const LineAccounting& accounting);
    /// The newest lines of `game`, newest first, optionally only those older than `before_id`.
    [[nodiscard]] QList<BacklogEntry> RecentLines(const QString& game, int limit,
                                                  std::optional<qint64> before_id = {});
    /// Lines of `game` whose text or speaker contains `query`, newest first.
    [[nodiscard]] QList<BacklogEntry> SearchLines(const QString& game, const QString& query,
                                                  int limit);
    [[nodiscard]] qint64 LineCount(const QString& game);

    // ---- ActivityStore ----
    std::optional<SessionRecord> OpenSession(const QString& game) override;
    qint64 CreateSession(const QString& game, qint64 started) override;
    ReadingCounters SessionCounters(qint64 id) override;
    void CloseSession(qint64 id, qint64 ended) override;
    void AddActivity(qint64 session_id, const QString& game, const QString& day,
                     const ReadingCounters& delta, qint64 last_activity) override;
    bool MarkSeen(const QString& game, const QString& source_id, qint64 timestamp) override;
    QStringList SessionSources(qint64 id) override;
    /**
     * Counts every kept line again by today's rule and takes the difference out of the day it
     * was counted in, so that what was read before the rule changed is measured like what is
     * read after it. Run once when a database of the older rule is opened; public for tests.
     */
    void RecountCharacters();
    /// Takes out of the counts what was counted twice: text of a line that already stood in
    /// the lines just before it in its session (ReadingRules::WithoutOverlap), as a long
    /// text scrolled a screenful at a time was counted before that rule. With `apply` false
    /// nothing is changed. Returns how many characters are (or would be) taken out. Doing it
    /// twice takes nothing out the second time.
    qint64 StripOverlaps(bool apply);

    [[nodiscard]] bool HasSeen(const QString& game, const QString& source_id);

    // ---- Activity queries ----
    /// Totals for `game`, or for every game when it is empty.
    [[nodiscard]] ReadingCounters Totals(const QString& game = {});
    /// Totals of one reading day, for `game` or for every game when it is empty.
    [[nodiscard]] ReadingCounters DayTotals(const QString& day, const QString& game = {});
    /// Totals per reading day, newest first, at most `limit` days.
    [[nodiscard]] QList<std::pair<QString, ReadingCounters>> DailyHistory(const QString& game = {},
                                                                         int limit = 30);
    /// Every game that has activity, with its totals, most read first.
    [[nodiscard]] QList<GameTotals> AllGameTotals();
    /// Sessions of `game` (or all games), newest first, each with its totals.
    [[nodiscard]] QList<SessionRecord> Sessions(const QString& game = {});
    /// Removes one session and everything it contributed to the totals.
    void DeleteSession(qint64 id);
    /**
     * Removes the sessions and daily totals of `game`, or of every game when it is empty. The
     * backlog and the seen sources stay, so passed text still counts as rereading afterwards.
     */
    void ClearActivity(const QString& game = {});

    // ---- Export ----
    /// Activity of every game: sessions with their per-day ledger, plus totals.
    [[nodiscard]] QJsonObject ExportActivityJson();
    /// What has been read, by reading day and game, with the games' names: what this install
    /// sends to the save sync service, whole each time ("days" and "games" of its record).
    [[nodiscard]] QJsonObject StatsRecord();
    /// One CSV row per session, oldest first.
    [[nodiscard]] QString ExportSessionsCsv();

private:
    QList<BacklogEntry> QueryLines(const char* where, const QList<QString>& arguments, int limit);
    ReadingCounters SumDaily(const char* where, const QList<QString>& arguments);
    QList<SessionRecord> SessionsLocked(const QString& game);
    QList<GameTotals> AllGameTotalsLocked();

    SqliteDb db;
    QMutex mutex;
};

} // namespace ReaderApp
