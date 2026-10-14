// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include "reader/desktop/reader_database.h"
#include "reader/desktop/settings.h"
#include "reader/desktop/stats.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

constexpr qint64 Minute = 60'000;
constexpr qint64 Hour = 60 * Minute;

TextLine Line(const QString& text, const QString& source,
              const QString& channel = QStringLiteral("dialogue")) {
    TextLine line;
    line.text = text;
    line.source_id = source;
    line.channel = channel;
    return line;
}

/// A tracker over a database in memory, with every moment in one made-up reading day.
struct Fixture {
    ReaderDatabase db{QStringLiteral(":memory:")};
    QString game{QStringLiteral("0100000000000001")};
    ActivityTracker tracker{db, game, [](qint64 ms) {
                                return QStringLiteral("day-%1").arg(ms / (24 * Hour));
                            }};
};

} // namespace

TEST_CASE("ReadingRules: only Japanese characters are counted", "[reader][stats]") {
    // Kana and kanji; not punctuation, the line break or a space.
    REQUIRE(ReadingRules::CountCharacters(Q(u"君の事は、\n忘れない。")) == 8);
    REQUIRE(ReadingRules::CountCharacters(Q(u"…君は旅人だ？\n道を見張れ")) == 10);
    // Nothing of signs, digits and Latin letters, in either width.
    REQUIRE(ReadingRules::CountCharacters(Q(u" a\tb　c d ")) == 0);
    REQUIRE(ReadingRules::CountCharacters(Q(u"！？「」（）、。・…―〜♪★ 123 １２３ abc ＡＢＣ %")) == 0);
    REQUIRE(ReadingRules::CountCharacters(Q(u"Lv.5の「剣」を3本")) == 4);
    // The long vowel mark and the iteration marks are part of words; so is half-width kana.
    REQUIRE(ReadingRules::CountCharacters(Q(u"コーヒー")) == 4);
    REQUIRE(ReadingRules::CountCharacters(Q(u"時々、ゝゞ")) == 4);
    REQUIRE(ReadingRules::CountCharacters(Q(u"ｶﾞｷ")) == 2);
    REQUIRE(ReadingRules::CountCharacters(Q(u"𠮷野家")) == 3);
    REQUIRE(ReadingRules::CountCharacters(QString()) == 0);
}

TEST_CASE("ReadingRules: a reading day starts at 04:01", "[reader][stats]") {
    const auto at = [](int hour, int minute, int second) {
        return QDateTime(QDate(2026, 3, 10), QTime(hour, minute, second)).toMSecsSinceEpoch();
    };
    REQUIRE(ReadingRules::ReadingDay(at(12, 0, 0)) == QStringLiteral("2026-03-10"));
    REQUIRE(ReadingRules::ReadingDay(at(4, 1, 0)) == QStringLiteral("2026-03-10"));
    REQUIRE(ReadingRules::ReadingDay(at(4, 0, 59)) == QStringLiteral("2026-03-09"));
    REQUIRE(ReadingRules::ReadingDay(at(0, 30, 0)) == QStringLiteral("2026-03-09"));
    REQUIRE_FALSE(ReadingRules::CharactersPerHour(100, 999).has_value());
    REQUIRE(ReadingRules::CharactersPerHour(100, 60'000) == 6000.0);
}

TEST_CASE("ActivityTracker: lines are counted; a source met again is rereading",
          "[reader][stats]") {
    Fixture f;
    qint64 now = 10 * Hour;
    const LineAccounting first = f.tracker.OnLine(Line(Q(u"こんにちは。"), QStringLiteral("a")), now);
    REQUIRE(first.counted);
    REQUIRE(first.characters == 5);
    REQUIRE_FALSE(first.reread);
    REQUIRE(first.session_id.has_value());
    // Shown again in the same session: the same reading, counted once. So are the same words
    // under another source, as a game gives them after a menu.
    const LineAccounting again =
        f.tracker.OnLine(Line(Q(u"こんにちは。"), QStringLiteral("a")), now + 1000);
    REQUIRE(again.reread);
    REQUIRE_FALSE(again.counted);
    REQUIRE_FALSE(
        f.tracker.OnLine(Line(Q(u" こんにちは。\n"), QStringLiteral("a2")), now + 1100).counted);
    // A line of signs alone is no reading.
    REQUIRE_FALSE(f.tracker.OnLine(Line(Q(u"……！？"), QStringLiteral("dots")), now + 1200).counted);
    const LineAccounting choice = f.tracker.OnLine(
        Line(Q(u"はい"), QStringLiteral("c"), QStringLiteral("choice")), now + 2000);
    REQUIRE(choice.counted);
    const LineAccounting blank = f.tracker.OnLine(Line(Q(u" 　\n"), QStringLiteral("b")), now);
    REQUIRE_FALSE(blank.counted);

    const ReadingCounters session = *f.tracker.Session();
    REQUIRE(session.characters == 5);
    REQUIRE(session.unique_characters == 5);
    REQUIRE(session.reread_characters == 0);
    REQUIRE(session.lines == 1);
    REQUIRE(session.choice_characters == 2);
    // Nothing is saved until it is flushed.
    REQUIRE(f.db.Totals().IsEmpty());
    f.tracker.Flush();
    REQUIRE(f.db.Totals() == session);
    REQUIRE(f.db.Totals(f.game) == session);
    REQUIRE(f.db.Totals(QStringLiteral("other")).IsEmpty());
    // Met again in a later session it is rereading, and counted as that.
    const LineAccounting later =
        f.tracker.OnLine(Line(Q(u"こんにちは。"), QStringLiteral("a")), now + 5 * Hour);
    REQUIRE(later.reread);
    REQUIRE(later.counted);
    REQUIRE(f.tracker.Session()->reread_characters == 5);
    // The app closed and opened within a session: the database knows what the session
    // counted, and a line of it is still not counted twice.
    {
        ReaderDatabase db(QStringLiteral(":memory:"));
        const auto day = [](qint64) { return QStringLiteral("day"); };
        ActivityTracker before(db, f.game, day);
        const TextLine line = Line(Q(u"道を見張れ"), QStringLiteral("s"));
        const LineAccounting counted = before.OnLine(line, now);
        db.InsertLine(f.game, line, counted);
        before.Flush();
        ActivityTracker after(db, f.game, day);
        REQUIRE_FALSE(after.OnLine(line, now + 1000).counted);
    }
}

TEST_CASE("ActivityTracker: time accrues only while reading, in small steps", "[reader][stats]") {
    Fixture f;
    qint64 now = 10 * Hour;
    // No narrative yet: time does not count.
    f.tracker.OnHeartbeat(now, true);
    f.tracker.OnLine(Line(Q(u"一行目"), QStringLiteral("a")), now);
    f.tracker.OnHeartbeat(now + 1000, true);
    REQUIRE(f.tracker.Session()->active_ms == 1000);
    // Not in front: nothing.
    f.tracker.OnHeartbeat(now + 2000, false);
    REQUIRE(f.tracker.Session()->active_ms == 1000);
    // A heartbeat that comes late credits five seconds at most.
    f.tracker.OnHeartbeat(now + 62'000, true);
    REQUIRE(f.tracker.Session()->active_ms == 6000);
    // Past the idle timeout nothing counts until there is activity again.
    f.tracker.OnHeartbeat(now + 6 * Minute, true);
    f.tracker.OnHeartbeat(now + 6 * Minute + 1000, true);
    REQUIRE(f.tracker.Session()->active_ms == 6000);
    f.tracker.OnLine(Line(Q(u"二行目"), QStringLiteral("b")), now + 7 * Minute);
    f.tracker.OnHeartbeat(now + 7 * Minute + 1000, true);
    REQUIRE(f.tracker.Session()->active_ms == 7000);
    // An interaction keeps the clock going without a new line.
    f.tracker.OnInteraction(now + 11 * Minute);
    f.tracker.OnHeartbeat(now + 15 * Minute, true);
    f.tracker.OnHeartbeat(now + 15 * Minute + 1000, true);
    REQUIRE(f.tracker.Session()->active_ms == 13'000);
    // Paused by hand: neither characters nor time.
    f.tracker.paused = true;
    REQUIRE_FALSE(f.tracker.OnLine(Line(Q(u"三行目"), QStringLiteral("c")), now + 16 * Minute).counted);
    f.tracker.OnHeartbeat(now + 16 * Minute, true);
    REQUIRE(f.tracker.Session()->active_ms == 13'000);
}

TEST_CASE("ActivityTracker: a long break starts a new session", "[reader][stats]") {
    Fixture f;
    qint64 now = 10 * Hour;
    const qint64 first = *f.tracker.OnLine(Line(Q(u"一"), QStringLiteral("a")), now).session_id;
    const qint64 same =
        *f.tracker.OnLine(Line(Q(u"二"), QStringLiteral("b")), now + 3 * Hour).session_id;
    REQUIRE(same == first);
    const qint64 next =
        *f.tracker.OnLine(Line(Q(u"三"), QStringLiteral("c")), now + 8 * Hour).session_id;
    REQUIRE(next != first);
    f.tracker.Flush();
    const QList<SessionRecord> sessions = f.db.Sessions();
    REQUIRE(sessions.size() == 2);
    // Newest first; the earlier one was closed at its last activity.
    REQUIRE(sessions.at(0).id == next);
    REQUIRE_FALSE(sessions.at(0).ended.has_value());
    REQUIRE(sessions.at(1).ended == now + 3 * Hour);
    REQUIRE(sessions.at(1).counters.characters == 2);

    // A tracker started later (the app was restarted) picks an open session up again.
    ActivityTracker restarted(f.db, f.game);
    const qint64 resumed =
        *restarted.OnLine(Line(Q(u"四"), QStringLiteral("d")), now + 9 * Hour).session_id;
    REQUIRE(resumed == next);
    REQUIRE(restarted.Session()->characters == 2);
}

TEST_CASE("ReaderDatabase: the backlog keeps lines with their ruby", "[reader][stats]") {
    ReaderDatabase db(QStringLiteral(":memory:"));
    const QString game = QStringLiteral("0100000000000001");
    db.SetGameTitle(game, Q(u"テスト"));
    TextLine line = Line(Q(u"漢字を読む"), QStringLiteral("s:1"));
    line.speaker = Q(u"先生");
    line.ruby = {{0, 2, Q(u"かんじ")}};
    line.meta.insert(QStringLiteral("k"), QStringLiteral("v"));
    line.timestamp = 123;
    const qint64 first = db.InsertLine(game, line, LineAccounting{5, false, true, 1});
    const qint64 second = db.InsertLine(
        game, Line(Q(u"100%_本気"), QStringLiteral("s:2")), LineAccounting{6, true, true, 1});
    db.InsertLine(QStringLiteral("other"), Line(Q(u"別"), QStringLiteral("x")), {});
    REQUIRE(db.LineCount(game) == 2);

    const QList<BacklogEntry> recent = db.RecentLines(game, 10);
    REQUIRE(recent.size() == 2);
    REQUIRE(recent.at(0).id == second);
    REQUIRE(recent.at(0).reread);
    REQUIRE(recent.at(1).id == first);
    REQUIRE(recent.at(1).line.text == Q(u"漢字を読む"));
    REQUIRE(recent.at(1).line.speaker == Q(u"先生"));
    REQUIRE(recent.at(1).line.ruby == line.ruby);
    REQUIRE(recent.at(1).line.meta == line.meta);
    REQUIRE(recent.at(1).line.timestamp == 123);
    REQUIRE(recent.at(1).characters == 5);
    REQUIRE(db.RecentLines(game, 10, second).size() == 1);
    REQUIRE(db.RecentLines(game, 1).size() == 1);

    REQUIRE(db.SearchLines(game, Q(u"読"), 10).size() == 1);
    REQUIRE(db.SearchLines(game, Q(u"先生"), 10).size() == 1);
    // What would be a wildcard is looked for as written.
    REQUIRE(db.SearchLines(game, QStringLiteral("%"), 10).size() == 1);
    REQUIRE(db.SearchLines(game, QStringLiteral("0%_"), 10).size() == 1);
    REQUIRE(db.SearchLines(game, QStringLiteral("_"), 10).size() == 1);
    REQUIRE(db.SearchLines(game, Q(u"無い"), 10).isEmpty());
}

TEST_CASE("ReaderDatabase: totals, deleting and clearing", "[reader][stats]") {
    Fixture f;
    qint64 now = 30 * Hour;
    f.db.SetGameTitle(f.game, Q(u"ゲーム"));
    f.tracker.OnLine(Line(Q(u"一二三"), QStringLiteral("a")), now);
    f.tracker.OnHeartbeat(now + 2000, true);
    // Later the same reading day, in the same session.
    f.tracker.OnLine(Line(Q(u"四五"), QStringLiteral("b")), now + 2 * Hour);
    f.tracker.Flush();
    ActivityTracker other(f.db, QStringLiteral("0100000000000002"),
                          [](qint64) { return QStringLiteral("day-1"); });
    other.OnLine(Line(Q(u"六七八九"), QStringLiteral("z")), now);
    other.Flush();

    REQUIRE(f.db.Totals().characters == 9);
    REQUIRE(f.db.Totals(f.game).characters == 5);
    REQUIRE(f.db.DayTotals(QStringLiteral("day-1")).characters == 9);
    REQUIRE(f.db.DayTotals(QStringLiteral("day-1"), f.game).characters == 5);
    const auto games = f.db.AllGameTotals();
    REQUIRE(games.size() == 2);
    REQUIRE(games.at(0).counters.characters == 5);
    REQUIRE(games.at(0).title == Q(u"ゲーム"));
    // A game without a known title goes by its ID.
    REQUIRE(games.at(1).title == QStringLiteral("0100000000000002"));
    REQUIRE(f.db.DailyHistory().size() == 1);
    REQUIRE(f.db.HasSeen(f.game, QStringLiteral("a")));
    REQUIRE_FALSE(f.db.HasSeen(f.game, QStringLiteral("z")));

    const QJsonObject exported = f.db.ExportActivityJson();
    REQUIRE(exported.value(QStringLiteral("sessions")).toArray().size() == 2);
    REQUIRE(exported.value(QStringLiteral("total")).toObject().value(QStringLiteral("characters")).toInt() == 9);
    const QString csv = f.db.ExportSessionsCsv();
    REQUIRE(csv.count(u'\n') == 3);
    REQUIRE(csv.contains(Q(u",\"ゲーム\",")));

    const qint64 session = f.db.Sessions(f.game).first().id;
    f.db.DeleteSession(session);
    REQUIRE(f.db.Totals().characters == 4);
    f.db.ClearActivity();
    REQUIRE(f.db.Totals().IsEmpty());
    REQUIRE(f.db.Sessions().isEmpty());
    // Clearing never touches what counts as read already.
    REQUIRE(f.db.HasSeen(f.game, QStringLiteral("a")));
}

TEST_CASE("ReaderDatabase: what was counted by the older rule is counted again",
          "[reader][stats]") {
    ReaderDatabase db(QStringLiteral(":memory:"));
    const QString game = QStringLiteral("0100000000000001");
    const qint64 ts = QDateTime(QDate(2026, 10, 1), QTime(12, 0)).toMSecsSinceEpoch();
    const QString day = ReadingRules::ReadingDay(ts);
    const qint64 session = db.CreateSession(game, ts);
    // As the older rule counted them: every character that is not a space.
    TextLine first = Line(Q(u"…君は旅人だ？"), QStringLiteral("a"));
    TextLine signs = Line(Q(u"……！"), QStringLiteral("b"));
    TextLine again = Line(Q(u"道を、見張れ。"), QStringLiteral("c"));
    TextLine choice = Line(Q(u"はい！"), QStringLiteral("d"), QStringLiteral("choice"));
    for (TextLine* line : {&first, &signs, &again, &choice}) {
        line->timestamp = ts;
    }
    db.InsertLine(game, first, LineAccounting{7, false, true, session});
    db.InsertLine(game, signs, LineAccounting{3, false, true, session});
    db.InsertLine(game, again, LineAccounting{7, true, true, session});
    db.InsertLine(game, choice, LineAccounting{3, false, true, session});
    db.AddActivity(session, game, day, ReadingCounters{60000, 17, 10, 7, 3, 3}, ts);

    db.RecountCharacters();
    // 君は旅人だ 5, nothing, 道を見張れ 5 (reread), はい 2 (a choice); the line of signs is gone.
    REQUIRE(db.Totals(game) == ReadingCounters{60000, 10, 5, 5, 2, 2});
    // A second time changes nothing.
    db.RecountCharacters();
    REQUIRE(db.Totals(game) == ReadingCounters{60000, 10, 5, 5, 2, 2});
}

TEST_CASE("ReaderDatabase: the record sent to the save sync service", "[reader][stats]") {
    QTemporaryDir folder;
    ReaderDatabase db(folder.filePath(QStringLiteral("reader.db")));
    const QString game = QStringLiteral("ULJS00219");
    const QString other = QStringLiteral("ULJM05000");
    REQUIRE(db.StatsRecord().value(QStringLiteral("days")).toArray().isEmpty());
    const qint64 s1 = db.CreateSession(game, 1000);
    const qint64 s2 = db.CreateSession(game, 5000);
    const qint64 s3 = db.CreateSession(other, 2000);
    db.AddActivity(s1, game, QStringLiteral("2026-10-01"), ReadingCounters{60000, 100, 80, 20, 5, 7}, 1500);
    db.AddActivity(s2, game, QStringLiteral("2026-10-01"), ReadingCounters{30000, 50, 50, 0, 2, 0}, 5800);
    db.AddActivity(s2, game, QStringLiteral("2026-10-02"), ReadingCounters{10000, 10, 10, 0, 1, 0}, 5900);
    db.AddActivity(s3, other, QStringLiteral("2026-10-02"), ReadingCounters{5000, 4, 4, 0, 1, 0}, 2100);
    db.SetGameTitle(game, Q(u"大逆転裁判1&2"));
    db.SetGameTitle(QStringLiteral("0100000000000009"), QStringLiteral("nothing was read of this"));

    const QJsonObject record = db.StatsRecord();
    const QJsonArray days = record.value(QStringLiteral("days")).toArray();
    REQUIRE(days.size() == 3);
    // By day, then by game; two sessions of a day are one entry that says so.
    const QJsonObject first = days.at(0).toObject();
    REQUIRE(first.value(QStringLiteral("day")).toString() == QStringLiteral("2026-10-01"));
    REQUIRE(first.value(QStringLiteral("game")).toString() == game);
    REQUIRE(first.value(QStringLiteral("active_ms")).toInteger() == 90000);
    REQUIRE(first.value(QStringLiteral("chars")).toInteger() == 150);
    REQUIRE(first.value(QStringLiteral("unique_chars")).toInteger() == 130);
    REQUIRE(first.value(QStringLiteral("reread_chars")).toInteger() == 20);
    REQUIRE(first.value(QStringLiteral("lines")).toInteger() == 7);
    REQUIRE(first.value(QStringLiteral("choice_chars")).toInteger() == 7);
    REQUIRE(first.value(QStringLiteral("sessions")).toInteger() == 2);
    REQUIRE(days.at(1).toObject().value(QStringLiteral("day")).toString() == QStringLiteral("2026-10-02"));
    REQUIRE(days.at(2).toObject().value(QStringLiteral("day")).toString() == QStringLiteral("2026-10-02"));
    // The names of the games that were read, and of no other.
    const QJsonObject games = record.value(QStringLiteral("games")).toObject();
    REQUIRE(games.size() == 1);
    REQUIRE(games.value(game).toString() == Q(u"大逆転裁判1&2"));
    // Cleared here, it is gone from the next record: the record is the whole of it.
    db.ClearActivity(game);
    REQUIRE(db.StatsRecord().value(QStringLiteral("days")).toArray().size() == 1);

    // The install's ID is made up once and kept.
    ReaderSettings settings(folder.filePath(QStringLiteral("reader.ini")));
    const QString install = settings.StatsInstall();
    REQUIRE(QRegularExpression(QStringLiteral("^[0-9a-f]{16}$")).match(install).hasMatch());
    REQUIRE(ReaderSettings(folder.filePath(QStringLiteral("reader.ini"))).StatsInstall() == install);
}

TEST_CASE("SessionSummary: a session in a few characters", "[reader][stats]") {
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    ReadingCounters counters;
    counters.characters = 1234;
    counters.active_ms = 30'000;
    // Too little reading for a speed worth showing.
    REQUIRE(SessionSummary::Format(counters) == Q(u"1,234字 · <1m"));
    counters.active_ms = 12 * Minute;
    REQUIRE(SessionSummary::Format(counters) == Q(u"1,234字 · 6,170/h · 12m"));
    REQUIRE(SessionSummary::Format(counters, true) == Q(u"paused · 1,234字 · 6,170/h · 12m"));
    REQUIRE(SessionSummary::Duration(65 * Minute) == QStringLiteral("1h05m"));
    REQUIRE(SessionSummary::Duration(0) == QStringLiteral("<1m"));
}

// A long text read by scrolling: every stop shows mostly what the stop before showed.
namespace {
const QString MailRow1 = Q(u"今日は実験の日だからラボに来てほしいのだが");
const QString MailRow2 = Q(u"途中でドクターペッパーを買ってきてくれると助かる");
const QString MailRow3 = Q(u"それから電話レンジの件はまだ誰にも話していない");
const QString MailRow4 = Q(u"くれぐれも機関の連中には気づかれないようにな");
const QString MailRow5 = Q(u"エルプサイコングルゥ");
} // namespace

TEST_CASE("ReadingRules: what stood in the lines just before is not new", "[reader][stats]") {
    using ReadingRules::CountCharacters;
    using ReadingRules::WithoutOverlap;
    const QString first = MailRow1 + MailRow2 + MailRow3;
    const QString second = MailRow2 + MailRow3 + MailRow4;
    REQUIRE(WithoutOverlap(first, {}) == first);
    REQUIRE(CountCharacters(WithoutOverlap(second, {first})) == CountCharacters(MailRow4));
    // Scrolled back up: nothing new. Scrolled on: the last rows only, whatever lies between.
    REQUIRE(CountCharacters(WithoutOverlap(first, {first, second})) == 0);
    REQUIRE(CountCharacters(WithoutOverlap(MailRow3 + MailRow4 + MailRow5, {first, second})) ==
            CountCharacters(MailRow5));
    // What two lines share by chance, a few characters, is read both times.
    const QString one = Q(u"それはどういう意味だ、オカリン");
    const QString other = Q(u"どういう意味だと聞かれても困る");
    REQUIRE(WithoutOverlap(other, {one}) == other);
    // Only the lines just before count: a text from long ago is read again.
    QStringList long_ago{second};
    for (int i = 0; i < ReadingRules::FreshWindow; ++i) {
        long_ago.append(QStringLiteral("line %1").arg(i));
    }
    REQUIRE(WithoutOverlap(second, long_ago) == second);
}

TEST_CASE("ActivityTracker: a text scrolled a screenful at a time is counted once", "[reader][stats]") {
    Fixture f;
    qint64 now = 10 * Hour;
    const int row = [](const QString& text) { return ReadingRules::CountCharacters(text); }(MailRow1);
    const auto count = [](const QString& text) { return ReadingRules::CountCharacters(text); };
    const LineAccounting a = f.tracker.OnLine(Line(MailRow1 + MailRow2 + MailRow3, QStringLiteral("m1")), now);
    REQUIRE(a.counted);
    REQUIRE(a.characters == row + count(MailRow2) + count(MailRow3));
    const LineAccounting b =
        f.tracker.OnLine(Line(MailRow2 + MailRow3 + MailRow4, QStringLiteral("m2")), now + 2000);
    REQUIRE(b.counted);
    REQUIRE(b.characters == count(MailRow4));
    // Back up: shown again, nothing counted.
    const LineAccounting c =
        f.tracker.OnLine(Line(MailRow1 + MailRow2 + MailRow3 + Q(u"。"), QStringLiteral("m3")), now + 4000);
    REQUIRE_FALSE(c.counted);
    const LineAccounting d =
        f.tracker.OnLine(Line(MailRow3 + MailRow4 + MailRow5, QStringLiteral("m4")), now + 6000);
    REQUIRE(d.characters == count(MailRow5));
    REQUIRE(f.tracker.Session()->characters ==
            count(MailRow1) + count(MailRow2) + count(MailRow3) + count(MailRow4) + count(MailRow5));
}

TEST_CASE("ReaderDatabase: what was counted twice is taken out, once", "[reader][stats]") {
    ReaderDatabase db(QStringLiteral(":memory:"));
    const QString game = QStringLiteral("ULJM00000");
    const auto count = [](const QString& text) { return ReadingRules::CountCharacters(text); };
    // As it was counted before the rule: every screenful in full.
    const qint64 ts = 1791600000000;
    const QString day = ReadingRules::ReadingDay(ts);
    const qint64 session = db.CreateSession(game, ts);
    const QStringList screens{MailRow1 + MailRow2 + MailRow3, MailRow2 + MailRow3 + MailRow4,
                              MailRow3 + MailRow4 + MailRow5, Q(u"別の話をしよう")};
    int counted = 0;
    for (int i = 0; i < screens.size(); ++i) {
        TextLine line = Line(screens[i], QStringLiteral("s%1").arg(i));
        line.timestamp = ts + i * 1000;
        const int characters = count(screens[i]);
        counted += characters;
        db.InsertLine(game, line, LineAccounting{characters, false, true, session});
        ReadingCounters delta;
        delta.characters = characters;
        delta.unique_characters = characters;
        delta.lines = 1;
        db.AddActivity(session, game, day, delta, line.timestamp);
    }
    REQUIRE(db.Totals(game).characters == counted);
    const int once = count(MailRow1) + count(MailRow2) + count(MailRow3) + count(MailRow4) + count(MailRow5) +
                     count(screens[3]);
    // Asked only: nothing changes.
    REQUIRE(db.StripOverlaps(false) == counted - once);
    REQUIRE(db.Totals(game).characters == counted);
    REQUIRE(db.StripOverlaps(true) == counted - once);
    REQUIRE(db.Totals(game).characters == once);
    REQUIRE(db.Totals(game).unique_characters == once);
    REQUIRE(db.Totals(game).lines == screens.size());
    REQUIRE(db.StripOverlaps(true) == 0);
    REQUIRE(db.Totals(game).characters == once);
}
