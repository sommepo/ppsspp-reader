// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutexLocker>
#include <QVariant>
#include "reader/desktop/reader_database.h"

namespace ReaderApp {

namespace {

constexpr const char* SumColumns =
    "COALESCE(SUM(active_ms), 0), COALESCE(SUM(chars), 0), COALESCE(SUM(unique_chars), 0), "
    "COALESCE(SUM(reread_chars), 0), COALESCE(SUM(lines), 0), COALESCE(SUM(choice_chars), 0)";

ReadingCounters CountersFrom(const SqliteDb::Statement& statement, int first) {
    return ReadingCounters{statement.Int(first),     statement.Int(first + 1),
                           statement.Int(first + 2), statement.Int(first + 3),
                           statement.Int(first + 4), statement.Int(first + 5)};
}

SessionRecord SessionFrom(const SqliteDb::Statement& statement, const ReadingCounters& counters) {
    SessionRecord record;
    record.id = statement.Int(0);
    record.game = statement.Text(1);
    record.started = statement.Int(2);
    record.last_activity = statement.Int(3);
    if (!statement.IsNull(4)) {
        record.ended = statement.Int(4);
    }
    record.counters = counters;
    return record;
}

QJsonObject CountersJson(const ReadingCounters& c) {
    QJsonObject out;
    out.insert(QStringLiteral("activeMs"), c.active_ms);
    out.insert(QStringLiteral("characters"), c.characters);
    out.insert(QStringLiteral("uniqueCharacters"), c.unique_characters);
    out.insert(QStringLiteral("rereadCharacters"), c.reread_characters);
    out.insert(QStringLiteral("lines"), c.lines);
    out.insert(QStringLiteral("choiceCharacters"), c.choice_characters);
    return out;
}

void BindAll(SqliteDb::Statement& statement, const QList<QString>& arguments) {
    for (int i = 0; i < arguments.size(); ++i) {
        statement.Bind(i + 1, arguments.at(i));
    }
}

} // namespace

ReaderDatabase::ReaderDatabase(const QString& path) {
    db.Open(path);
    if (db.UserVersion() == 0) {
        SqliteDb::Transaction transaction(db);
        db.Exec("CREATE TABLE IF NOT EXISTS lines ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "game TEXT NOT NULL,"
                "session_id INTEGER,"
                "ts INTEGER NOT NULL,"
                "channel TEXT NOT NULL,"
                "speaker TEXT NOT NULL,"
                "text TEXT NOT NULL,"
                "ruby TEXT NOT NULL,"
                "source_id TEXT NOT NULL,"
                "meta TEXT NOT NULL,"
                "chars INTEGER NOT NULL,"
                "reread INTEGER NOT NULL)");
        db.Exec("CREATE INDEX IF NOT EXISTS lines_game ON lines(game, id)");
        db.Exec("CREATE TABLE IF NOT EXISTS seen ("
                "game TEXT NOT NULL,"
                "source_id TEXT NOT NULL,"
                "first_ts INTEGER NOT NULL,"
                "count INTEGER NOT NULL,"
                "PRIMARY KEY (game, source_id)) WITHOUT ROWID");
        db.Exec("CREATE TABLE IF NOT EXISTS sessions ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "game TEXT NOT NULL,"
                "started INTEGER NOT NULL,"
                "last_activity INTEGER NOT NULL,"
                "ended INTEGER)");
        db.Exec("CREATE INDEX IF NOT EXISTS sessions_game ON sessions(game, id)");
        db.Exec("CREATE TABLE IF NOT EXISTS daily ("
                "session_id INTEGER NOT NULL,"
                "day TEXT NOT NULL,"
                "game TEXT NOT NULL,"
                "active_ms INTEGER NOT NULL DEFAULT 0,"
                "chars INTEGER NOT NULL DEFAULT 0,"
                "unique_chars INTEGER NOT NULL DEFAULT 0,"
                "reread_chars INTEGER NOT NULL DEFAULT 0,"
                "lines INTEGER NOT NULL DEFAULT 0,"
                "choice_chars INTEGER NOT NULL DEFAULT 0,"
                "PRIMARY KEY (session_id, day)) WITHOUT ROWID");
        db.Exec("CREATE INDEX IF NOT EXISTS daily_game ON daily(game, day)");
        db.Exec("CREATE TABLE IF NOT EXISTS games ("
                "game TEXT PRIMARY KEY, title TEXT NOT NULL) WITHOUT ROWID");
        transaction.Commit();
        db.SetUserVersion(1);
    }
    if (db.UserVersion() == 1) {
        // Version 2 counts Japanese characters only: the same tables, counted again.
        RecountCharacters();
        db.SetUserVersion(2);
    }
}

void ReaderDatabase::SetGameTitle(const QString& game, const QString& title) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "INSERT INTO games(game, title) VALUES(?, ?) "
                                      "ON CONFLICT(game) DO UPDATE SET title = excluded.title");
    statement.Bind(1, game).Bind(2, title).Run();
}

qint64 ReaderDatabase::InsertLine(const QString& game, const TextLine& line,
                                  const LineAccounting& accounting) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(
        db, "INSERT INTO lines(game, session_id, ts, channel, speaker, text, ruby, source_id, "
            "meta, chars, reread) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    statement.Bind(1, game);
    if (accounting.session_id) {
        statement.Bind(2, *accounting.session_id);
    } else {
        statement.BindNull(2);
    }
    QJsonObject meta;
    for (auto it = line.meta.constBegin(); it != line.meta.constEnd(); ++it) {
        meta.insert(it.key(), it.value());
    }
    statement.Bind(3, line.timestamp)
        .Bind(4, line.channel)
        .Bind(5, line.speaker)
        .Bind(6, line.text)
        .Bind(7, QString::fromUtf8(
                     QJsonDocument(TextLine::RubyToJson(line.ruby)).toJson(QJsonDocument::Compact)))
        .Bind(8, line.source_id)
        .Bind(9, QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)))
        .Bind(10, accounting.characters)
        .Bind(11, accounting.reread ? 1 : 0)
        .Run();
    return db.LastInsertId();
}

QList<BacklogEntry> ReaderDatabase::RecentLines(const QString& game, int limit,
                                                std::optional<qint64> before_id) {
    QMutexLocker lock(&mutex);
    if (!before_id) {
        return QueryLines("game = ?", {game}, limit);
    }
    return QueryLines("game = ? AND id < ?", {game, QString::number(*before_id)}, limit);
}

QList<BacklogEntry> ReaderDatabase::SearchLines(const QString& game, const QString& query,
                                                int limit) {
    QString escaped = query;
    escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'%', QStringLiteral("\\%"));
    escaped.replace(u'_', QStringLiteral("\\_"));
    const QString pattern = u'%' + escaped + u'%';
    QMutexLocker lock(&mutex);
    return QueryLines("game = ? AND (text LIKE ? ESCAPE '\\' OR speaker LIKE ? ESCAPE '\\')",
                      {game, pattern, pattern}, limit);
}

qint64 ReaderDatabase::LineCount(const QString& game) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "SELECT COUNT(*) FROM lines WHERE game = ?");
    return statement.Bind(1, game).Step() ? statement.Int(0) : 0;
}

QList<BacklogEntry> ReaderDatabase::QueryLines(const char* where, const QList<QString>& arguments,
                                               int limit) {
    const std::string sql =
        std::string("SELECT id, game, ts, channel, speaker, text, ruby, source_id, meta, chars, "
                    "reread FROM lines WHERE ") +
        where + " ORDER BY id DESC LIMIT " + std::to_string(limit);
    SqliteDb::Statement statement(db, sql.c_str());
    BindAll(statement, arguments);
    QList<BacklogEntry> out;
    while (statement.Step()) {
        BacklogEntry entry;
        entry.id = statement.Int(0);
        entry.game = statement.Text(1);
        entry.line.timestamp = statement.Int(2);
        entry.line.audio_position = -1;
        entry.line.channel = statement.Text(3);
        entry.line.speaker = statement.Text(4);
        entry.line.text = statement.Text(5);
        entry.line.ruby = TextLine::RubyFromJson(
            QJsonDocument::fromJson(statement.Text(6).toUtf8()).array(), entry.line.text);
        entry.line.source_id = statement.Text(7);
        const QJsonObject meta = QJsonDocument::fromJson(statement.Text(8).toUtf8()).object();
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
            entry.line.meta.insert(it.key(), it.value().toVariant().toString());
        }
        entry.characters = static_cast<int>(statement.Int(9));
        entry.reread = statement.Int(10) != 0;
        out.append(entry);
    }
    return out;
}

std::optional<SessionRecord> ReaderDatabase::OpenSession(const QString& game) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "SELECT id, game, started, last_activity, ended FROM "
                                      "sessions WHERE game = ? AND ended IS NULL "
                                      "ORDER BY id DESC LIMIT 1");
    if (!statement.Bind(1, game).Step()) {
        return std::nullopt;
    }
    return SessionFrom(statement, ReadingCounters{});
}

qint64 ReaderDatabase::CreateSession(const QString& game, qint64 started) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(
        db, "INSERT INTO sessions(game, started, last_activity) VALUES(?, ?, ?)");
    statement.Bind(1, game).Bind(2, started).Bind(3, started).Run();
    return db.LastInsertId();
}

ReadingCounters ReaderDatabase::SessionCounters(qint64 id) {
    QMutexLocker lock(&mutex);
    return SumDaily("session_id = ?", {QString::number(id)});
}

void ReaderDatabase::CloseSession(qint64 id, qint64 ended) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db,
                                  "UPDATE sessions SET ended = ? WHERE id = ? AND ended IS NULL");
    statement.Bind(1, ended).Bind(2, id).Run();
}

void ReaderDatabase::AddActivity(qint64 session_id, const QString& game, const QString& day,
                                 const ReadingCounters& delta, qint64 last_activity) {
    QMutexLocker lock(&mutex);
    SqliteDb::Transaction transaction(db);
    {
        SqliteDb::Statement statement(
            db, "INSERT INTO daily(session_id, day, game, active_ms, chars, unique_chars, "
                "reread_chars, lines, choice_chars) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(session_id, day) DO UPDATE SET "
                "active_ms = active_ms + excluded.active_ms, "
                "chars = chars + excluded.chars, "
                "unique_chars = unique_chars + excluded.unique_chars, "
                "reread_chars = reread_chars + excluded.reread_chars, "
                "lines = lines + excluded.lines, "
                "choice_chars = choice_chars + excluded.choice_chars");
        statement.Bind(1, session_id)
            .Bind(2, day)
            .Bind(3, game)
            .Bind(4, delta.active_ms)
            .Bind(5, delta.characters)
            .Bind(6, delta.unique_characters)
            .Bind(7, delta.reread_characters)
            .Bind(8, delta.lines)
            .Bind(9, delta.choice_characters)
            .Run();
    }
    {
        SqliteDb::Statement statement(
            db, "UPDATE sessions SET last_activity = MAX(last_activity, ?) WHERE id = ?");
        statement.Bind(1, last_activity).Bind(2, session_id).Run();
    }
    transaction.Commit();
}

void ReaderDatabase::RecountCharacters() {
    QMutexLocker lock(&mutex);
    struct Change {
        qint64 id;
        qint64 session;
        QString day;
        bool choice;
        bool reread;
        int was;
        int now;
    };
    QList<Change> changes;
    {
        SqliteDb::Statement statement(db, "SELECT id, session_id, ts, channel, text, chars, reread "
                                          "FROM lines WHERE session_id IS NOT NULL");
        while (statement.Step()) {
            const int now = ReadingRules::CountCharacters(statement.Text(4));
            const int was = static_cast<int>(statement.Int(5));
            if (now != was) {
                changes.append(Change{statement.Int(0), statement.Int(1),
                                      ReadingRules::ReadingDay(statement.Int(2)),
                                      statement.Text(3) == QStringLiteral("choice"),
                                      statement.Int(6) != 0, was, now});
            }
        }
    }
    if (changes.isEmpty()) {
        return;
    }
    SqliteDb::Transaction transaction(db);
    for (const Change& change : changes) {
        const qint64 less = change.was - change.now;
        SqliteDb::Statement(db, "UPDATE lines SET chars = ? WHERE id = ?")
            .Bind(1, static_cast<qint64>(change.now))
            .Bind(2, change.id)
            .Run();
        if (change.choice) {
            SqliteDb::Statement(db, "UPDATE daily SET choice_chars = MAX(0, choice_chars - ?) "
                                    "WHERE session_id = ? AND day = ?")
                .Bind(1, less)
                .Bind(2, change.session)
                .Bind(3, change.day)
                .Run();
        } else {
            const std::string sql =
                std::string("UPDATE daily SET chars = MAX(0, chars - ?), ") +
                (change.reread ? "reread_chars = MAX(0, reread_chars - ?)"
                               : "unique_chars = MAX(0, unique_chars - ?)") +
                ", lines = MAX(0, lines - ?) WHERE session_id = ? AND day = ?";
            SqliteDb::Statement(db, sql.c_str())
                .Bind(1, less)
                .Bind(2, less)
                .Bind(3, static_cast<qint64>(change.now == 0 ? 1 : 0))
                .Bind(4, change.session)
                .Bind(5, change.day)
                .Run();
        }
    }
    transaction.Commit();
}

qint64 ReaderDatabase::StripOverlaps(bool apply) {
    QMutexLocker lock(&mutex);
    struct Change {
        qint64 id;
        qint64 session;
        QString day;
        bool reread;
        int was;
        int now;
    };
    QList<Change> changes;
    qint64 taken = 0;
    {
        SqliteDb::Statement statement(db, "SELECT id, session_id, ts, channel, text, chars, reread "
                                          "FROM lines WHERE session_id IS NOT NULL ORDER BY session_id, id");
        qint64 session = -1;
        QStringList before;
        while (statement.Step()) {
            if (statement.Int(1) != session) {
                session = statement.Int(1);
                before.clear();
            }
            const QString text = statement.Text(4);
            if (statement.Text(3) != QStringLiteral("choice")) {
                const int was = static_cast<int>(statement.Int(5));
                const int now = std::min(
                    was, ReadingRules::CountCharacters(ReadingRules::WithoutOverlap(text, before)));
                if (now < was) {
                    taken += was - now;
                    changes.append(Change{statement.Int(0), session, ReadingRules::ReadingDay(statement.Int(2)),
                                          statement.Int(6) != 0, was, now});
                }
            }
            // The same text again says nothing new about what came before.
            if (!before.contains(text)) {
                before.append(text);
                if (before.size() > ReadingRules::FreshWindow) {
                    before.removeFirst();
                }
            }
        }
    }
    if (!apply || changes.isEmpty()) {
        return taken;
    }
    SqliteDb::Transaction transaction(db);
    for (const Change& change : changes) {
        const qint64 less = change.was - change.now;
        SqliteDb::Statement(db, "UPDATE lines SET chars = ? WHERE id = ?")
            .Bind(1, static_cast<qint64>(change.now))
            .Bind(2, change.id)
            .Run();
        const std::string sql =
            std::string("UPDATE daily SET chars = MAX(0, chars - ?), ") +
            (change.reread ? "reread_chars = MAX(0, reread_chars - ?)"
                           : "unique_chars = MAX(0, unique_chars - ?)") +
            ", lines = MAX(0, lines - ?) WHERE session_id = ? AND day = ?";
        SqliteDb::Statement(db, sql.c_str())
            .Bind(1, less)
            .Bind(2, less)
            .Bind(3, static_cast<qint64>(change.now == 0 ? 1 : 0))
            .Bind(4, change.session)
            .Bind(5, change.day)
            .Run();
    }
    transaction.Commit();
    return taken;
}

QStringList ReaderDatabase::SessionSources(qint64 id) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(
        db, "SELECT DISTINCT source_id FROM lines WHERE session_id = ? AND source_id != ''");
    statement.Bind(1, id);
    QStringList out;
    while (statement.Step()) {
        out.append(statement.Text(0));
    }
    return out;
}

bool ReaderDatabase::MarkSeen(const QString& game, const QString& source_id, qint64 timestamp) {
    QMutexLocker lock(&mutex);
    bool seen_before;
    {
        SqliteDb::Statement statement(db, "SELECT 1 FROM seen WHERE game = ? AND source_id = ?");
        seen_before = statement.Bind(1, game).Bind(2, source_id).Step();
    }
    SqliteDb::Statement statement(
        db, "INSERT INTO seen(game, source_id, first_ts, count) VALUES(?, ?, ?, 1) "
            "ON CONFLICT(game, source_id) DO UPDATE SET count = count + 1");
    statement.Bind(1, game).Bind(2, source_id).Bind(3, timestamp).Run();
    return seen_before;
}

bool ReaderDatabase::HasSeen(const QString& game, const QString& source_id) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "SELECT 1 FROM seen WHERE game = ? AND source_id = ?");
    return statement.Bind(1, game).Bind(2, source_id).Step();
}

ReadingCounters ReaderDatabase::Totals(const QString& game) {
    QMutexLocker lock(&mutex);
    return game.isEmpty() ? SumDaily(nullptr, {}) : SumDaily("game = ?", {game});
}

ReadingCounters ReaderDatabase::DayTotals(const QString& day, const QString& game) {
    QMutexLocker lock(&mutex);
    return game.isEmpty() ? SumDaily("day = ?", {day})
                          : SumDaily("day = ? AND game = ?", {day, game});
}

QList<std::pair<QString, ReadingCounters>> ReaderDatabase::DailyHistory(const QString& game,
                                                                        int limit) {
    QMutexLocker lock(&mutex);
    const std::string sql = std::string("SELECT day, ") + SumColumns + " FROM daily " +
                            (game.isEmpty() ? "" : "WHERE game = ? ") +
                            "GROUP BY day ORDER BY day DESC LIMIT " + std::to_string(limit);
    SqliteDb::Statement statement(db, sql.c_str());
    if (!game.isEmpty()) {
        statement.Bind(1, game);
    }
    QList<std::pair<QString, ReadingCounters>> out;
    while (statement.Step()) {
        out.append({statement.Text(0), CountersFrom(statement, 1)});
    }
    return out;
}

QList<GameTotals> ReaderDatabase::AllGameTotals() {
    QMutexLocker lock(&mutex);
    return AllGameTotalsLocked();
}

QList<GameTotals> ReaderDatabase::AllGameTotalsLocked() {
    const std::string sql =
        std::string("SELECT d.game, COALESCE(g.title, d.game), ") + SumColumns +
        " FROM daily d LEFT JOIN games g ON g.game = d.game "
        "GROUP BY d.game ORDER BY SUM(d.chars) DESC";
    SqliteDb::Statement statement(db, sql.c_str());
    QList<GameTotals> out;
    while (statement.Step()) {
        out.append(GameTotals{statement.Text(0), statement.Text(1), CountersFrom(statement, 2)});
    }
    return out;
}

QList<SessionRecord> ReaderDatabase::Sessions(const QString& game) {
    QMutexLocker lock(&mutex);
    return SessionsLocked(game);
}

QList<SessionRecord> ReaderDatabase::SessionsLocked(const QString& game) {
    const std::string sql =
        std::string("SELECT s.id, s.game, s.started, s.last_activity, s.ended, "
                    "COALESCE(SUM(d.active_ms), 0), COALESCE(SUM(d.chars), 0), "
                    "COALESCE(SUM(d.unique_chars), 0), COALESCE(SUM(d.reread_chars), 0), "
                    "COALESCE(SUM(d.lines), 0), COALESCE(SUM(d.choice_chars), 0) "
                    "FROM sessions s LEFT JOIN daily d ON d.session_id = s.id ") +
        (game.isEmpty() ? "" : "WHERE s.game = ? ") + "GROUP BY s.id ORDER BY s.id DESC";
    SqliteDb::Statement statement(db, sql.c_str());
    if (!game.isEmpty()) {
        statement.Bind(1, game);
    }
    QList<SessionRecord> out;
    while (statement.Step()) {
        out.append(SessionFrom(statement, CountersFrom(statement, 5)));
    }
    return out;
}

void ReaderDatabase::DeleteSession(qint64 id) {
    QMutexLocker lock(&mutex);
    SqliteDb::Transaction transaction(db);
    for (const char* sql :
         {"DELETE FROM daily WHERE session_id = ?", "DELETE FROM sessions WHERE id = ?"}) {
        SqliteDb::Statement statement(db, sql);
        statement.Bind(1, id).Run();
    }
    transaction.Commit();
}

void ReaderDatabase::ClearActivity(const QString& game) {
    QMutexLocker lock(&mutex);
    SqliteDb::Transaction transaction(db);
    if (game.isEmpty()) {
        db.Exec("DELETE FROM daily");
        db.Exec("DELETE FROM sessions");
    } else {
        for (const char* sql :
             {"DELETE FROM daily WHERE game = ?", "DELETE FROM sessions WHERE game = ?"}) {
            SqliteDb::Statement statement(db, sql);
            statement.Bind(1, game).Run();
        }
    }
    transaction.Commit();
}

ReadingCounters ReaderDatabase::SumDaily(const char* where, const QList<QString>& arguments) {
    const std::string sql = std::string("SELECT ") + SumColumns + " FROM daily" +
                            (where ? std::string(" WHERE ") + where : std::string());
    SqliteDb::Statement statement(db, sql.c_str());
    BindAll(statement, arguments);
    return statement.Step() ? CountersFrom(statement, 0) : ReadingCounters{};
}

QJsonObject ReaderDatabase::StatsRecord() {
    QMutexLocker lock(&mutex);
    QJsonObject games;
    {
        SqliteDb::Statement statement(
            db, "SELECT game, title FROM games WHERE game IN (SELECT DISTINCT game FROM daily)");
        while (statement.Step()) {
            games.insert(statement.Text(0), statement.Text(1));
        }
    }
    QJsonArray days;
    {
        SqliteDb::Statement statement(
            db, "SELECT day, game, SUM(active_ms), SUM(chars), SUM(unique_chars), "
                "SUM(reread_chars), SUM(lines), SUM(choice_chars), COUNT(DISTINCT session_id) "
                "FROM daily GROUP BY day, game ORDER BY day, game");
        while (statement.Step()) {
            QJsonObject day;
            day.insert(QStringLiteral("day"), statement.Text(0));
            day.insert(QStringLiteral("game"), statement.Text(1));
            day.insert(QStringLiteral("active_ms"), statement.Int(2));
            day.insert(QStringLiteral("chars"), statement.Int(3));
            day.insert(QStringLiteral("unique_chars"), statement.Int(4));
            day.insert(QStringLiteral("reread_chars"), statement.Int(5));
            day.insert(QStringLiteral("lines"), statement.Int(6));
            day.insert(QStringLiteral("choice_chars"), statement.Int(7));
            day.insert(QStringLiteral("sessions"), statement.Int(8));
            days.append(day);
        }
    }
    QJsonObject record;
    record.insert(QStringLiteral("games"), games);
    record.insert(QStringLiteral("days"), days);
    return record;
}

QJsonObject ReaderDatabase::ExportActivityJson() {
    QMutexLocker lock(&mutex);
    QJsonObject root;
    // The same format as the 3DS sibling writes, so that one tool reads both.
    root.insert(QStringLiteral("format"), QStringLiteral("azahar-reader.activity"));
    root.insert(QStringLiteral("version"), 1);
    QHash<QString, QString> titles;
    {
        SqliteDb::Statement statement(db, "SELECT game, title FROM games");
        while (statement.Step()) {
            titles.insert(statement.Text(0), statement.Text(1));
        }
    }
    QHash<qint64, QJsonArray> ledger;
    {
        SqliteDb::Statement statement(
            db, "SELECT session_id, day, active_ms, chars, unique_chars, reread_chars, lines, "
                "choice_chars FROM daily ORDER BY session_id, day");
        while (statement.Step()) {
            QJsonObject day = CountersJson(CountersFrom(statement, 2));
            day.insert(QStringLiteral("day"), statement.Text(1));
            ledger[statement.Int(0)].append(day);
        }
    }
    QList<SessionRecord> oldest_first = SessionsLocked({});
    std::reverse(oldest_first.begin(), oldest_first.end());
    QJsonArray sessions;
    for (const SessionRecord& s : oldest_first) {
        QJsonObject session = CountersJson(s.counters);
        session.insert(QStringLiteral("id"), s.id);
        session.insert(QStringLiteral("game"), s.game);
        session.insert(QStringLiteral("title"), titles.value(s.game, s.game));
        session.insert(QStringLiteral("started"), s.started);
        session.insert(QStringLiteral("lastActivity"), s.last_activity);
        session.insert(QStringLiteral("ended"), s.ended ? QJsonValue(*s.ended) : QJsonValue());
        session.insert(QStringLiteral("days"), ledger.value(s.id));
        sessions.append(session);
    }
    root.insert(QStringLiteral("sessions"), sessions);
    QJsonArray games;
    for (const GameTotals& g : AllGameTotalsLocked()) {
        QJsonObject game = CountersJson(g.counters);
        game.insert(QStringLiteral("game"), g.game);
        game.insert(QStringLiteral("title"), g.title);
        games.append(game);
    }
    root.insert(QStringLiteral("games"), games);
    root.insert(QStringLiteral("total"), CountersJson(SumDaily(nullptr, {})));
    return root;
}

QString ReaderDatabase::ExportSessionsCsv() {
    QMutexLocker lock(&mutex);
    QString out = QStringLiteral(
        "session,game,title,started,last_activity,ended,active_seconds,characters,"
        "unique_characters,reread_characters,lines,choice_characters\n");
    QHash<QString, QString> titles;
    {
        SqliteDb::Statement statement(db, "SELECT game, title FROM games");
        while (statement.Step()) {
            titles.insert(statement.Text(0), statement.Text(1));
        }
    }
    QList<SessionRecord> oldest_first = SessionsLocked({});
    std::reverse(oldest_first.begin(), oldest_first.end());
    for (const SessionRecord& s : oldest_first) {
        QString title = titles.value(s.game, s.game);
        title.replace(u'"', QStringLiteral("\"\""));
        out += QString::number(s.id) + u',' + s.game + QStringLiteral(",\"") + title +
               QStringLiteral("\",") + QString::number(s.started) + u',' +
               QString::number(s.last_activity) + u',' +
               (s.ended ? QString::number(*s.ended) : QString()) + u',' +
               QString::number(s.counters.active_ms / 1000) + u',' +
               QString::number(s.counters.characters) + u',' +
               QString::number(s.counters.unique_characters) + u',' +
               QString::number(s.counters.reread_characters) + u',' +
               QString::number(s.counters.lines) + u',' +
               QString::number(s.counters.choice_characters) + u'\n';
    }
    return out;
}

} // namespace ReaderApp
