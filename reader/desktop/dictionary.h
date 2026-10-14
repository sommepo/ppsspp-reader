// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include "reader/desktop/deinflector.h"
#include "reader/desktop/sqlite_db.h"

namespace ReaderApp {

/// An installed dictionary. Lower `priority` is shown first.
struct DictionaryInfo {
    qint64 id{};
    QString title;
    QString revision;
    QString author;
    QString description;
    QString frequency_mode;
    int priority{};
    bool enabled{};
    qint64 term_count{};
    qint64 meta_count{};
};

/// One dictionary row for a term. `glossary_json` is the entry's glossary array as JSON.
struct TermEntry {
    qint64 dictionary_id{};
    QString dictionary;
    int priority{};
    QString expression;
    QString reading;
    QString definition_tags;
    QString rules;
    int score{};
    QString glossary_json;
    qint64 sequence{};
    QString term_tags;
};

/// A frequency, pitch or other "meta" row for a term. `data` is its JSON value.
struct TermMeta {
    QString dictionary;
    QString frequency_mode;
    QString expression;
    QString mode;
    QString data;
};

/**
 * Dictionaries imported from Yomitan-format archives, stored for lookup by expression or
 * reading. The file has the Android app's tables and the same packing of glossaries, so a
 * reader_dict.db made by either works in the other.
 */
class DictionaryDatabase {
public:
    static constexpr const char* FileName = "reader_dict.db";

    /// Opens (and creates) the database file, or ":memory:".
    explicit DictionaryDatabase(const QString& path);

    [[nodiscard]] QList<DictionaryInfo> Dictionaries();
    [[nodiscard]] bool HasEnabledDictionaries();
    void SetEnabled(qint64 id, bool enabled);
    /// Stores a new display order: the first id becomes the highest priority.
    void SetOrder(const QList<qint64>& ids);
    void Delete(qint64 id);

    /// Term rows of enabled dictionaries whose expression or reading is one of `terms`.
    [[nodiscard]] QList<TermEntry> FindTerms(const QStringList& terms);
    /// Frequency and pitch rows of enabled dictionaries for the given expressions.
    [[nodiscard]] QList<TermMeta> FindMeta(const QStringList& expressions);

    /// Compresses a glossary for storage; short ones are stored as they are.
    [[nodiscard]] static QByteArray Pack(const QString& json);
    [[nodiscard]] static QString Unpack(const QByteArray& blob);

    /// For the importer, which writes under one transaction of its own.
    [[nodiscard]] SqliteDb& Db() {
        return db;
    }
    /// Held around every use of the connection.
    [[nodiscard]] QMutex& Mutex() {
        return mutex;
    }

private:
    SqliteDb db;
    QMutex mutex;
};

/// How common a term is according to one frequency dictionary.
struct Frequency {
    QString dictionary;
    QString display;
    std::optional<double> value;
    bool rank_based{};
};

/// Pitch accent of one reading: the mora after which the pitch drops (0 = none).
struct PitchAccent {
    QString dictionary;
    QString reading;
    QList<int> positions;
};

/// One headword found at the looked-up position, with everything the dictionaries say of it.
struct LookupResult {
    QString expression;
    QString reading;
    /// The part of the scanned text this headword covers.
    QString matched_text;
    /// Inflections undone to reach the headword, outermost first.
    QStringList reasons;
    /// Dictionary entries, most preferred dictionary first.
    QList<TermEntry> entries;
    QList<Frequency> frequencies;
    QList<PitchAccent> pitches;

    /// The reading to show: empty readings mean the expression is already kana.
    [[nodiscard]] QString DisplayReading() const {
        return (reading.isEmpty() || reading == expression) ? QString() : reading;
    }
};

/**
 * Finds the dictionary words that start at the beginning of a piece of text: the longest
 * stretch first, each stretch tried as written, in the other kana script and deinflected.
 */
class DictionaryLookup {
public:
    /// Longest stretch of text tried as one word.
    static constexpr int MaxScanLength = 20;

    explicit DictionaryLookup(DictionaryDatabase& database_) : database{database_} {}

    /// The words starting at the first character of `text`: at most `max_results`, best first.
    [[nodiscard]] QList<LookupResult> Lookup(const QString& text, int max_results = 24);

    /**
     * A single rank for sorting cards by frequency: the harmonic mean of the term's ranks in
     * rank-based frequency dictionaries, or nothing when there is none.
     */
    [[nodiscard]] static std::optional<qint64> HarmonicRank(const QList<Frequency>& frequencies);

private:
    DictionaryDatabase& database;
    Deinflector deinflector;
};

/**
 * Imports a dictionary archive in the Yomitan format (index.json plus term_bank_N.json and
 * term_meta_bank_N.json files). Kanji and tag banks are skipped.
 */
class DictionaryImporter {
public:
    struct Result {
        bool ok{};
        QString title;
        qint64 terms{};
        qint64 metas{};
        /// What went wrong, when not ok.
        QString error;
    };

    /// Called with the number of banks done, the total and the dictionary's title.
    using Progress = std::function<void(int done, int total, const QString& title)>;
    /// Returning false abandons the import and leaves the database unchanged.
    using KeepGoing = std::function<bool()>;

    explicit DictionaryImporter(DictionaryDatabase& database_) : database{database_} {}

    [[nodiscard]] Result Import(const QString& archive_path, const Progress& progress = {},
                                const KeepGoing& keep_going = {});

private:
    DictionaryDatabase& database;
};

} // namespace ReaderApp
