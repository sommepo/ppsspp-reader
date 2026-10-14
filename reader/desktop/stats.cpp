// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <utility>
#include <vector>
#include "reader/desktop/stats.h"

namespace ReaderApp {

std::optional<double> ReadingCounters::CharactersPerHour() const {
    return ReadingRules::CharactersPerHour(characters, active_ms);
}

int ReadingRules::CountCharacters(const QString& text) {
    int count = 0;
    for (int i = 0; i < text.size(); i += CharCountAt(text, i)) {
        if (IsJapanese(CodePointAt(text, i))) {
            ++count;
        }
    }
    return count;
}

namespace {
/// The longest stretch that stands in both: where it begins in `a` and how long it is.
std::pair<qsizetype, qsizetype> LongestShared(const QString& a, const QString& b) {
    std::vector<qsizetype> last(static_cast<std::size_t>(b.size()) + 1, 0), now(last.size(), 0);
    qsizetype best = 0, best_end = 0;
    for (qsizetype i = 1; i <= a.size(); ++i) {
        for (qsizetype j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                now[j] = last[j - 1] + 1;
                if (now[j] > best) {
                    best = now[j];
                    best_end = i;
                }
            } else {
                now[j] = 0;
            }
        }
        std::swap(last, now);
    }
    return {best_end - best, best};
}

/// Whether the two may share a stretch of FreshOverlap characters: a quick look before the
/// slow one. Every such stretch holds one of the pieces looked for.
bool MayShare(const QString& a, const QString& b) {
    constexpr qsizetype Piece = 8, Step = ReadingRules::FreshOverlap - Piece + 1;
    for (qsizetype i = 0; i + Piece <= a.size(); i += Step) {
        if (b.contains(QStringView{a}.mid(i, Piece))) {
            return true;
        }
    }
    return false;
}
} // namespace

QString ReadingRules::WithoutOverlap(const QString& text, const QStringList& before) {
    QString fresh = text;
    if (text.size() < FreshOverlap || before.isEmpty()) {
        return fresh;
    }
    const qsizetype first = std::max<qsizetype>(0, before.size() - FreshWindow);
    // A stretch at a time, the longest first, until nothing long enough is left.
    for (int round = 0; round < 8; ++round) {
        qsizetype at = 0, length = 0;
        for (qsizetype i = first; i < before.size(); ++i) {
            if (!MayShare(fresh, before[i])) {
                continue;
            }
            const auto [where, shared] = LongestShared(fresh, before[i]);
            if (shared > length) {
                at = where;
                length = shared;
            }
        }
        if (length < FreshOverlap) {
            break;
        }
        // A line break in its place keeps what stood on either side apart.
        fresh.replace(at, length, QStringLiteral("\n"));
    }
    return fresh;
}

bool ReadingRules::IsJapanese(char32_t cp) {
    const auto in = [cp](char32_t first, char32_t last) { return cp >= first && cp <= last; };
    return in(0x3041, 0x3096) || in(0x309D, 0x309F) ||   // hiragana
           in(0x30A1, 0x30FA) || in(0x30FC, 0x30FF) ||   // katakana, ー
           in(0x31F0, 0x31FF) ||                         // small katakana for Ainu
           in(0xFF66, 0xFF9D) ||                         // half-width katakana
           in(0x3005, 0x3007) ||                         // 々 〆 〇
           in(0x3400, 0x4DBF) || in(0x4E00, 0x9FFF) || in(0xF900, 0xFAFF) || // kanji
           in(0x20000, 0x323AF);                         // kanji beyond the basic plane
}

QString ReadingRules::ReadingDay(qint64 epoch_ms) {
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(epoch_ms).toLocalTime();
    // Wall-clock arithmetic, as the Android app does it: the date of the local time moved
    // back by four hours and a minute.
    const QDateTime shifted = QDateTime(local.date(), local.time(), QTimeZone::UTC)
                                  .addSecs(-static_cast<qint64>(DayStartMinutes) * 60);
    return shifted.date().toString(Qt::ISODate);
}

std::optional<double> ReadingRules::CharactersPerHour(qint64 characters, qint64 active_ms) {
    if (active_ms < 1000) {
        return std::nullopt;
    }
    return static_cast<double>(characters) * 3'600'000.0 / static_cast<double>(active_ms);
}

ActivityTracker::ActivityTracker(ActivityStore& store_, QString game_, DayOf day_of_,
                                 qint64 idle_timeout_ms_)
    : idle_timeout_ms{idle_timeout_ms_}, store{store_}, game{std::move(game_)},
      day_of{day_of_ ? std::move(day_of_) : DayOf{ReadingRules::ReadingDay}} {}

LineAccounting ActivityTracker::OnLine(const TextLine& line, qint64 now) {
    const int characters = ReadingRules::CountCharacters(line.text);
    // What stood in the lines just before (a long text scrolled a screenful at a time) was
    // read then: only the rest counts. A choice is what it is each time.
    const int fresh = line.IsChoice() || paused || characters == 0
                          ? characters
                          : ReadingRules::CountCharacters(ReadingRules::WithoutOverlap(line.text, recent_texts));
    if (paused || characters == 0) {
        return LineAccounting{characters, false, false, std::nullopt};
    }
    // Coming back from a pause in reading: time before this line is not reading time.
    const bool resuming = !session_id || now - last_activity > idle_timeout_ms;
    const qint64 id = EnsureSession(now);
    if (IsRepeat(line) || fresh == 0) {
        // Shown again (a menu closed over it, a save loaded, the game repeating itself): the
        // reader is still there, and nothing new was read.
        last_activity = now;
        if (resuming || last_heartbeat == 0) {
            last_heartbeat = now;
        }
        return LineAccounting{characters, true, false, std::nullopt};
    }
    const bool reread = store.MarkSeen(game, line.source_id, now);
    ReadingCounters delta;
    if (line.IsChoice()) {
        delta.choice_characters = fresh;
    } else {
        has_narrative = true;
        delta.characters = fresh;
        delta.unique_characters = reread ? 0 : fresh;
        delta.reread_characters = reread ? fresh : 0;
        delta.lines = 1;
    }
    Add(now, delta);
    last_activity = now;
    if (resuming || last_heartbeat == 0) {
        last_heartbeat = now;
    }
    return LineAccounting{fresh, reread, true, id};
}

bool ActivityTracker::IsRepeat(const TextLine& line) {
    const QString text = line.text.trimmed();
    const bool known = (!line.source_id.isEmpty() && counted_sources.contains(line.source_id)) ||
                       recent_texts.contains(text);
    if (!line.source_id.isEmpty()) {
        counted_sources.insert(line.source_id);
    }
    if (!known) {
        recent_texts.append(text);
        if (recent_texts.size() > RecentTexts) {
            recent_texts.removeFirst();
        }
    }
    return known;
}

void ActivityTracker::OnInteraction(qint64 now) {
    if (paused || !session_id) {
        return;
    }
    if (now - last_activity <= idle_timeout_ms) {
        last_activity = now;
    }
}

void ActivityTracker::OnHeartbeat(qint64 now, bool reading) {
    const qint64 previous = last_heartbeat;
    last_heartbeat = now;
    if (paused || !reading || !has_narrative || !session_id || previous == 0) {
        return;
    }
    if (now <= previous || now - last_activity > idle_timeout_ms) {
        return;
    }
    if (now - last_activity > ReadingRules::SessionGapMs) {
        return;
    }
    ReadingCounters delta;
    delta.active_ms = std::min(now - previous, ReadingRules::MaxHeartbeatMs);
    Add(now, delta);
}

void ActivityTracker::Flush() {
    if (!session_id || pending.isEmpty()) {
        return;
    }
    for (const auto& [day, delta] : pending) {
        store.AddActivity(*session_id, game, day, delta, last_activity);
    }
    pending.clear();
}

void ActivityTracker::Reset() {
    pending.clear();
    counted_sources.clear();
    recent_texts.clear();
    session_id.reset();
    session.reset();
    has_narrative = false;
    last_activity = 0;
    last_heartbeat = 0;
}

void ActivityTracker::Add(qint64 now, const ReadingCounters& delta) {
    const QString day = day_of(now);
    bool found = false;
    for (auto& [pending_day, counters] : pending) {
        if (pending_day == day) {
            counters = counters + delta;
            found = true;
            break;
        }
    }
    if (!found) {
        pending.append({day, delta});
    }
    session = session.value_or(ReadingCounters{}) + delta;
}

qint64 ActivityTracker::EnsureSession(qint64 now) {
    if (session_id) {
        if (now - last_activity < ReadingRules::SessionGapMs) {
            return *session_id;
        }
        Flush();
        store.CloseSession(*session_id, last_activity);
        session_id.reset();
    }
    const std::optional<SessionRecord> open = store.OpenSession(game);
    qint64 id;
    if (open && now - open->last_activity < ReadingRules::SessionGapMs) {
        id = open->id;
    } else {
        if (open) {
            store.CloseSession(open->id, open->last_activity);
        }
        id = store.CreateSession(game, now);
    }
    session_id = id;
    const QStringList before = store.SessionSources(id);
    counted_sources = QSet<QString>(before.begin(), before.end());
    recent_texts.clear();
    session = store.SessionCounters(id);
    last_activity = now;
    return id;
}

QString SessionSummary::Format(const ReadingCounters& counters, bool paused) {
    const QLocale locale;
    QString out;
    if (paused) {
        out += QStringLiteral("paused · ");
    }
    out += locale.toString(counters.characters) + QChar(u'字');
    const std::optional<double> speed = counters.CharactersPerHour();
    if (speed && counters.active_ms >= SpeedAfterMs) {
        // To the nearest ten: the last digit would change with every line.
        out += QStringLiteral(" · ") +
               locale.toString(static_cast<qint64>(std::llround(*speed / 10.0)) * 10) +
               QStringLiteral("/h");
    }
    out += QStringLiteral(" · ") + Duration(counters.active_ms);
    return out;
}

QString SessionSummary::Duration(qint64 active_ms) {
    const qint64 minutes = active_ms / 60000;
    if (minutes < 1) {
        return QStringLiteral("<1m");
    }
    if (minutes < 60) {
        return QStringLiteral("%1m").arg(minutes);
    }
    return QStringLiteral("%1h%2m").arg(minutes / 60).arg(minutes % 60, 2, 10, QChar(u'0'));
}

} // namespace ReaderApp
