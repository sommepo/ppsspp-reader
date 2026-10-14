// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSet>
#include <zlib.h>
#include "reader/desktop/dictionary.h"
#include "reader/desktop/japanese_text.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp {

namespace {

constexpr int MaxParameters = 400;
constexpr char PackRaw = 0;
constexpr char PackDeflate = 1;
constexpr int PackThreshold = 96;

std::string Placeholders(qsizetype count) {
    std::string out;
    for (qsizetype i = 0; i < count; ++i) {
        out += i == 0 ? "?" : ",?";
    }
    return out;
}

} // namespace

DictionaryDatabase::DictionaryDatabase(const QString& path) {
    db.Open(path);
    if (db.UserVersion() == 0) {
        SqliteDb::Transaction transaction(db);
        db.Exec("CREATE TABLE IF NOT EXISTS dictionaries ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "title TEXT NOT NULL UNIQUE,"
                "revision TEXT NOT NULL,"
                "author TEXT NOT NULL,"
                "description TEXT NOT NULL,"
                "frequency_mode TEXT NOT NULL,"
                "priority INTEGER NOT NULL,"
                "enabled INTEGER NOT NULL DEFAULT 1,"
                "terms INTEGER NOT NULL DEFAULT 0,"
                "metas INTEGER NOT NULL DEFAULT 0)");
        db.Exec("CREATE TABLE IF NOT EXISTS terms ("
                "id INTEGER PRIMARY KEY,"
                "dict INTEGER NOT NULL,"
                "expression TEXT NOT NULL,"
                "reading TEXT NOT NULL,"
                "def_tags TEXT NOT NULL,"
                "rules TEXT NOT NULL,"
                "score INTEGER NOT NULL,"
                "glossary BLOB NOT NULL,"
                "sequence INTEGER NOT NULL,"
                "term_tags TEXT NOT NULL)");
        db.Exec("CREATE INDEX IF NOT EXISTS terms_expression ON terms(expression)");
        db.Exec("CREATE INDEX IF NOT EXISTS terms_reading ON terms(reading)");
        db.Exec("CREATE TABLE IF NOT EXISTS term_meta ("
                "id INTEGER PRIMARY KEY,"
                "dict INTEGER NOT NULL,"
                "expression TEXT NOT NULL,"
                "mode TEXT NOT NULL,"
                "data TEXT NOT NULL)");
        db.Exec("CREATE INDEX IF NOT EXISTS term_meta_expression ON term_meta(expression)");
        transaction.Commit();
        db.SetUserVersion(1);
    }
}

QList<DictionaryInfo> DictionaryDatabase::Dictionaries() {
    QMutexLocker lock(&mutex);
    QList<DictionaryInfo> out;
    SqliteDb::Statement statement(
        db, "SELECT id, title, revision, author, description, frequency_mode, priority, enabled, "
            "terms, metas FROM dictionaries ORDER BY priority, id");
    while (statement.Step()) {
        out.append(DictionaryInfo{
            statement.Int(0), statement.Text(1), statement.Text(2), statement.Text(3),
            statement.Text(4), statement.Text(5), static_cast<int>(statement.Int(6)),
            statement.Int(7) != 0, statement.Int(8), statement.Int(9)});
    }
    return out;
}

bool DictionaryDatabase::HasEnabledDictionaries() {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "SELECT 1 FROM dictionaries WHERE enabled = 1 LIMIT 1");
    return statement.Step();
}

void DictionaryDatabase::SetEnabled(qint64 id, bool enabled) {
    QMutexLocker lock(&mutex);
    SqliteDb::Statement statement(db, "UPDATE dictionaries SET enabled = ? WHERE id = ?");
    statement.Bind(1, enabled ? 1 : 0).Bind(2, id).Run();
}

void DictionaryDatabase::SetOrder(const QList<qint64>& ids) {
    QMutexLocker lock(&mutex);
    SqliteDb::Transaction transaction(db);
    SqliteDb::Statement statement(db, "UPDATE dictionaries SET priority = ? WHERE id = ?");
    for (int index = 0; index < ids.size(); ++index) {
        statement.Bind(1, index).Bind(2, ids.at(index)).Run();
    }
    transaction.Commit();
}

void DictionaryDatabase::Delete(qint64 id) {
    QMutexLocker lock(&mutex);
    SqliteDb::Transaction transaction(db);
    for (const char* sql : {"DELETE FROM terms WHERE dict = ?", "DELETE FROM term_meta WHERE dict = ?",
                            "DELETE FROM dictionaries WHERE id = ?"}) {
        SqliteDb::Statement statement(db, sql);
        statement.Bind(1, id).Run();
    }
    transaction.Commit();
}

QList<TermEntry> DictionaryDatabase::FindTerms(const QStringList& terms) {
    QList<TermEntry> out;
    if (terms.isEmpty()) {
        return out;
    }
    QMutexLocker lock(&mutex);
    QSet<qint64> seen;
    for (qsizetype from = 0; from < terms.size(); from += MaxParameters) {
        const qsizetype count = std::min<qsizetype>(MaxParameters, terms.size() - from);
        for (const char* column : {"expression", "reading"}) {
            const std::string sql =
                std::string("SELECT t.id, t.dict, d.title, d.priority, t.expression, t.reading, "
                            "t.def_tags, t.rules, t.score, t.glossary, t.sequence, t.term_tags "
                            "FROM terms t JOIN dictionaries d ON d.id = t.dict WHERE t.") +
                column + " IN (" + Placeholders(count) + ") AND d.enabled = 1";
            SqliteDb::Statement statement(db, sql.c_str());
            for (qsizetype i = 0; i < count; ++i) {
                statement.Bind(static_cast<int>(i) + 1, terms.at(from + i));
            }
            while (statement.Step()) {
                const qint64 row = statement.Int(0);
                if (seen.contains(row)) {
                    continue;
                }
                seen.insert(row);
                TermEntry entry;
                entry.dictionary_id = statement.Int(1);
                entry.dictionary = statement.Text(2);
                entry.priority = static_cast<int>(statement.Int(3));
                entry.expression = statement.Text(4);
                entry.reading = statement.Text(5);
                entry.definition_tags = statement.Text(6);
                entry.rules = statement.Text(7);
                entry.score = static_cast<int>(statement.Int(8));
                entry.glossary_json = Unpack(statement.Blob(9));
                entry.sequence = statement.Int(10);
                entry.term_tags = statement.Text(11);
                out.append(entry);
            }
        }
    }
    return out;
}

QList<TermMeta> DictionaryDatabase::FindMeta(const QStringList& expressions) {
    QList<TermMeta> out;
    if (expressions.isEmpty()) {
        return out;
    }
    QMutexLocker lock(&mutex);
    for (qsizetype from = 0; from < expressions.size(); from += MaxParameters) {
        const qsizetype count = std::min<qsizetype>(MaxParameters, expressions.size() - from);
        const std::string sql =
            "SELECT d.title, d.frequency_mode, m.expression, m.mode, m.data "
            "FROM term_meta m JOIN dictionaries d ON d.id = m.dict "
            "WHERE m.expression IN (" +
            Placeholders(count) + ") AND d.enabled = 1 ORDER BY d.priority, d.id";
        SqliteDb::Statement statement(db, sql.c_str());
        for (qsizetype i = 0; i < count; ++i) {
            statement.Bind(static_cast<int>(i) + 1, expressions.at(from + i));
        }
        while (statement.Step()) {
            out.append(TermMeta{statement.Text(0), statement.Text(1), statement.Text(2),
                                statement.Text(3), statement.Text(4)});
        }
    }
    return out;
}

QByteArray DictionaryDatabase::Pack(const QString& json) {
    const QByteArray raw = json.toUtf8();
    if (raw.size() < PackThreshold) {
        return QByteArray(1, PackRaw) + raw;
    }
    uLongf length = compressBound(static_cast<uLong>(raw.size()));
    QByteArray out(static_cast<qsizetype>(length) + 1, Qt::Uninitialized);
    out[0] = PackDeflate;
    if (compress2(reinterpret_cast<Bytef*>(out.data() + 1), &length,
                  reinterpret_cast<const Bytef*>(raw.constData()), static_cast<uLong>(raw.size()),
                  Z_BEST_SPEED) != Z_OK) {
        return QByteArray(1, PackRaw) + raw;
    }
    out.resize(static_cast<qsizetype>(length) + 1);
    return out;
}

QString DictionaryDatabase::Unpack(const QByteArray& blob) {
    if (blob.isEmpty()) {
        return QStringLiteral("[]");
    }
    if (blob.at(0) == PackRaw) {
        return QString::fromUtf8(blob.constData() + 1, blob.size() - 1);
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return QStringLiteral("[]");
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(blob.constData() + 1));
    stream.avail_in = static_cast<uInt>(blob.size() - 1);
    QByteArray out;
    char buffer[4096];
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            break;
        }
        out.append(buffer, static_cast<qsizetype>(sizeof(buffer) - stream.avail_out));
    }
    inflateEnd(&stream);
    return QString::fromUtf8(out);
}

// ---- Lookup ----

namespace {

struct Candidate {
    int source_length;
    Deinflection deinflection;
};

bool IsBetter(const Candidate& a, const Candidate& b) {
    if (a.source_length != b.source_length) {
        return a.source_length > b.source_length;
    }
    return a.deinflection.reasons.size() < b.deinflection.reasons.size();
}

/// The leading part of `text` worth scanning: up to the first break, at most a few words.
QString Scannable(const QString& text) {
    int end = 0;
    while (end < text.size() && end < DictionaryLookup::MaxScanLength) {
        const char32_t cp = CodePointAt(text, end);
        if (!JapaneseText::IsTermChar(cp) && cp != U'々' && cp != U'〇' && cp != U'・' &&
            cp != U'～' && cp != U'〜') {
            break;
        }
        end += CharCountAt(text, end);
    }
    return text.left(end);
}

/// `source` as written plus the spellings a dictionary may use for it.
QStringList Variants(const QString& source) {
    QStringList out;
    const auto add = [&out](const QString& value) {
        if (!out.contains(value)) {
            out.append(value);
        }
    };
    add(source);
    add(JapaneseText::KatakanaToHiragana(source));
    const QString ascii = JapaneseText::FullWidthToAscii(source);
    add(ascii);
    if (std::any_of(ascii.begin(), ascii.end(),
                    [](QChar c) { return c.unicode() >= u'A' && c.unicode() <= u'Z'; })) {
        add(ascii.toLower());
    }
    return out;
}

/// A meta row's JSON value, whatever its type (a bare number or string is allowed).
QJsonValue ParseValue(const QString& json) {
    const QByteArray wrapped = "[" + json.trimmed().toUtf8() + "]";
    const QJsonDocument document = QJsonDocument::fromJson(wrapped);
    if (!document.isArray() || document.array().isEmpty()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    return document.array().at(0);
}

QString FormatNumber(double value) {
    if (value == std::floor(value) && std::isfinite(value)) {
        return QString::number(static_cast<qint64>(value));
    }
    return QString::number(value, 'g', 15);
}

QList<Frequency> Frequencies(const QList<TermMeta>& meta, const QString& reading) {
    QList<Frequency> out;
    for (const TermMeta& m : meta) {
        if (m.mode != QStringLiteral("freq")) {
            continue;
        }
        QJsonValue value = ParseValue(m.data);
        if (value.isObject() && value.toObject().contains(QStringLiteral("frequency"))) {
            // {reading, frequency}: only for the matching reading.
            const QJsonObject object = value.toObject();
            const QString for_reading = object.value(QStringLiteral("reading")).toString();
            if (!reading.isEmpty() && !for_reading.isEmpty() &&
                JapaneseText::KatakanaToHiragana(for_reading) !=
                    JapaneseText::KatakanaToHiragana(reading)) {
                continue;
            }
            value = object.value(QStringLiteral("frequency"));
        }
        std::optional<double> number;
        QString display;
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            if (object.value(QStringLiteral("value")).isDouble()) {
                number = object.value(QStringLiteral("value")).toDouble();
            }
            display = object.value(QStringLiteral("displayValue")).toString();
            if (display.isEmpty() && number) {
                display = FormatNumber(*number);
            }
        } else if (value.isDouble()) {
            number = value.toDouble();
            display = FormatNumber(*number);
        } else if (value.isString()) {
            display = value.toString();
            bool ok = false;
            const double parsed = display.toDouble(&ok);
            if (ok) {
                number = parsed;
            }
        } else {
            continue;
        }
        if (display.isEmpty()) {
            continue;
        }
        out.append(Frequency{m.dictionary, display, number,
                             m.frequency_mode != QStringLiteral("occurrence-based")});
    }
    return out;
}

QList<PitchAccent> Pitches(const QList<TermMeta>& meta, const QString& reading) {
    QList<PitchAccent> out;
    const QString wanted = JapaneseText::KatakanaToHiragana(reading);
    for (const TermMeta& m : meta) {
        if (m.mode != QStringLiteral("pitch")) {
            continue;
        }
        const QJsonValue value = ParseValue(m.data);
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        const QString for_reading = object.value(QStringLiteral("reading")).toString();
        if (JapaneseText::KatakanaToHiragana(for_reading) != wanted) {
            continue;
        }
        QList<int> positions;
        for (const QJsonValue& pitch : object.value(QStringLiteral("pitches")).toArray()) {
            const QJsonValue position = pitch.toObject().value(QStringLiteral("position"));
            if (position.isDouble()) {
                positions.append(position.toInt());
            }
        }
        if (!positions.isEmpty()) {
            out.append(PitchAccent{m.dictionary, for_reading, positions});
        }
    }
    return out;
}

/// A headword is one expression + reading. Kept, per headword: the best way it matched.
struct Headword {
    QString expression;
    QString reading;
    int source_length{};
    QStringList reasons;
    bool validated{};
    QList<TermEntry> entries;

    [[nodiscard]] int BestPriority() const {
        int best = std::numeric_limits<int>::max();
        for (const TermEntry& entry : entries) {
            best = std::min(best, entry.priority);
        }
        return best;
    }
    [[nodiscard]] int BestScore() const {
        int best = std::numeric_limits<int>::min();
        for (const TermEntry& entry : entries) {
            best = std::max(best, entry.score);
        }
        return best;
    }
};

} // namespace

QList<LookupResult> DictionaryLookup::Lookup(const QString& text, int max_results) {
    const QString scan = Scannable(text);
    if (scan.isEmpty()) {
        return {};
    }

    // Every (prefix, deinflection) pair, keyed by the term to look for.
    QHash<QString, QList<Candidate>> by_term;
    QStringList terms; // in the order first met, so that results do not depend on hashing
    for (int length = static_cast<int>(scan.size()); length > 0; --length) {
        if (scan.at(length - 1).isHighSurrogate()) {
            continue;
        }
        const QString source = scan.left(length);
        for (const QString& variant : Variants(source)) {
            for (const Deinflection& deinflection : deinflector.Deinflect(variant)) {
                auto found = by_term.find(deinflection.term);
                if (found == by_term.end()) {
                    found = by_term.insert(deinflection.term, {});
                    terms.append(deinflection.term);
                }
                found->append(Candidate{length, deinflection});
            }
        }
    }

    const QList<TermEntry> rows = database.FindTerms(terms);
    if (rows.isEmpty()) {
        return {};
    }

    QList<Headword> headwords;
    QHash<QString, int> headword_index;
    for (const TermEntry& row : rows) {
        const int word_class = WordClass::Parse(row.rules);
        const Candidate* best = nullptr;
        bool best_validated = false;
        for (const QString& key : {row.expression, row.reading}) {
            if (key.isEmpty()) {
                continue;
            }
            const auto found = by_term.constFind(key);
            if (found == by_term.constEnd()) {
                continue;
            }
            for (const Candidate& candidate : *found) {
                const int required = candidate.deinflection.conditions;
                const bool exact = required == 0;
                const bool class_matches = exact || (required & word_class) != 0;
                // A dictionary without word classes cannot confirm an inflection itself; such
                // rows are only kept when another dictionary confirms the headword.
                const bool acceptable = class_matches || word_class == 0;
                if (!acceptable) {
                    continue;
                }
                const bool better = best == nullptr || (class_matches && !best_validated) ||
                                    (class_matches == best_validated && IsBetter(candidate, *best));
                if (better) {
                    best = &candidate;
                    best_validated = class_matches;
                }
            }
        }
        if (best == nullptr) {
            continue;
        }
        const QString key = row.expression + QChar(u'\0') + row.reading;
        auto index = headword_index.constFind(key);
        if (index == headword_index.constEnd()) {
            headwords.append(Headword{row.expression, row.reading, 0, {}, false, {}});
            index = headword_index.insert(key, static_cast<int>(headwords.size()) - 1);
        }
        Headword& headword = headwords[*index];
        headword.entries.append(row);
        if (best_validated) {
            const bool improves =
                !headword.validated || best->source_length > headword.source_length ||
                (best->source_length == headword.source_length &&
                 best->deinflection.reasons.size() < headword.reasons.size());
            if (improves) {
                headword.source_length = best->source_length;
                headword.reasons = best->deinflection.reasons;
            }
            headword.validated = true;
        } else if (!headword.validated && best->source_length > headword.source_length) {
            headword.source_length = best->source_length;
            headword.reasons = best->deinflection.reasons;
        }
    }

    QList<Headword> confirmed;
    for (const Headword& headword : headwords) {
        if (headword.validated) {
            confirmed.append(headword);
        }
    }
    if (confirmed.isEmpty()) {
        return {};
    }
    std::stable_sort(confirmed.begin(), confirmed.end(), [](const Headword& a, const Headword& b) {
        if (a.source_length != b.source_length) {
            return a.source_length > b.source_length;
        }
        if (a.reasons.size() != b.reasons.size()) {
            return a.reasons.size() < b.reasons.size();
        }
        const int a_priority = a.BestPriority();
        const int b_priority = b.BestPriority();
        if (a_priority != b_priority) {
            return a_priority < b_priority;
        }
        const int a_score = a.BestScore();
        const int b_score = b.BestScore();
        if (a_score != b_score) {
            return a_score > b_score;
        }
        return a.expression < b.expression;
    });
    if (confirmed.size() > max_results) {
        confirmed.resize(max_results);
    }

    QStringList expressions;
    for (const Headword& headword : confirmed) {
        if (!expressions.contains(headword.expression)) {
            expressions.append(headword.expression);
        }
    }
    QHash<QString, QList<TermMeta>> metas;
    for (const TermMeta& meta : database.FindMeta(expressions)) {
        metas[meta.expression].append(meta);
    }

    QList<LookupResult> out;
    for (Headword& headword : confirmed) {
        const QList<TermMeta> meta = metas.value(headword.expression);
        std::stable_sort(headword.entries.begin(), headword.entries.end(),
                         [](const TermEntry& a, const TermEntry& b) {
                             if (a.priority != b.priority) {
                                 return a.priority < b.priority;
                             }
                             return a.score > b.score;
                         });
        LookupResult result;
        result.expression = headword.expression;
        result.reading = headword.reading;
        result.matched_text =
            text.left(std::min<qsizetype>(headword.source_length, text.size()));
        result.reasons = headword.reasons;
        result.entries = headword.entries;
        result.frequencies = Frequencies(meta, headword.reading);
        result.pitches =
            Pitches(meta, headword.reading.isEmpty() ? headword.expression : headword.reading);
        out.append(result);
    }
    return out;
}

std::optional<qint64> DictionaryLookup::HarmonicRank(const QList<Frequency>& frequencies) {
    int count = 0;
    double sum = 0.0;
    for (const Frequency& frequency : frequencies) {
        if (frequency.rank_based && frequency.value && *frequency.value > 0) {
            ++count;
            sum += 1.0 / *frequency.value;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return static_cast<qint64>(std::llround(count / sum));
}

} // namespace ReaderApp
