// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <utility>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QRegularExpression>
#include <quazip.h>
#include <quazipfile.h>
#include "reader/desktop/dictionary.h"

namespace ReaderApp {

namespace {

/// Carries the reason an import was given up.
struct ImportFailure {
    QString message;
};

QByteArray ReadEntry(QuaZip& zip, const QString& name) {
    if (!zip.setCurrentFile(name)) {
        throw ImportFailure{QStringLiteral("%1 cannot be read from the archive").arg(name)};
    }
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly)) {
        throw ImportFailure{QStringLiteral("%1 cannot be read from the archive").arg(name)};
    }
    return file.readAll();
}

/// A string or number as text; anything else as nothing.
QString Text(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == std::floor(number)) {
            return QString::number(static_cast<qint64>(number));
        }
        return QString::number(number, 'g', 15);
    }
    return {};
}

qint64 Number(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        return static_cast<qint64>(value.toString().toDouble());
    }
    return 0;
}

/// Any JSON value as JSON text: a meta row's value may be a bare number or string.
QString JsonText(const QJsonValue& value) {
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

QJsonArray ParseBank(QuaZip& zip, const QString& name) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(ReadEntry(zip, name), &error);
    if (!document.isArray()) {
        throw ImportFailure{QStringLiteral("%1 is not valid: %2").arg(name, error.errorString())};
    }
    return document.array();
}

} // namespace

DictionaryImporter::Result DictionaryImporter::Import(const QString& archive_path,
                                                      const Progress& progress,
                                                      const KeepGoing& keep_going) {
    Result result;
    try {
        QuaZip zip(archive_path);
        if (!zip.open(QuaZip::mdUnzip)) {
            throw ImportFailure{QStringLiteral("The file cannot be opened as a zip archive")};
        }
        const QStringList entries = zip.getFileNameList();
        QString index_name;
        for (const QString& name : entries) {
            if (name == QStringLiteral("index.json") || name.endsWith(QStringLiteral("/index.json"))) {
                if (index_name.isEmpty() || name.size() < index_name.size()) {
                    index_name = name;
                }
            }
        }
        if (index_name.isEmpty()) {
            throw ImportFailure{QStringLiteral("Not a dictionary archive: index.json is missing")};
        }
        const QString prefix = index_name.left(index_name.size() - 10); // "index.json"
        const QJsonDocument index_document = QJsonDocument::fromJson(ReadEntry(zip, index_name));
        if (!index_document.isObject()) {
            throw ImportFailure{QStringLiteral("index.json cannot be read")};
        }
        const QJsonObject index = index_document.object();
        const QString title = index.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty()) {
            throw ImportFailure{QStringLiteral("index.json has no title")};
        }
        result.title = title;
        const int format = index.value(QStringLiteral("format"))
                               .toInt(index.value(QStringLiteral("version")).toInt(3));
        if (format != 1 && format != 3) {
            throw ImportFailure{QStringLiteral("Unsupported dictionary format %1").arg(format)};
        }

        const auto banks = [&entries, &prefix](const QString& kind) {
            const QRegularExpression pattern(QRegularExpression::anchoredPattern(
                QRegularExpression::escape(prefix) + kind + QStringLiteral("_(\\d+)\\.json")));
            QList<std::pair<int, QString>> found;
            for (const QString& name : entries) {
                const QRegularExpressionMatch match = pattern.match(name);
                if (match.hasMatch()) {
                    found.append({match.captured(1).toInt(), name});
                }
            }
            std::sort(found.begin(), found.end());
            QStringList names;
            for (const auto& [number, name] : found) {
                names.append(name);
            }
            return names;
        };
        const QStringList term_banks = banks(QStringLiteral("term_bank"));
        const QStringList meta_banks = banks(QStringLiteral("term_meta_bank"));
        if (term_banks.isEmpty() && meta_banks.isEmpty()) {
            throw ImportFailure{
                QStringLiteral("\"%1\" has no term or frequency data").arg(title)};
        }
        const int total = static_cast<int>(term_banks.size() + meta_banks.size());
        const auto wanted = [&keep_going] {
            if (keep_going && !keep_going()) {
                throw ImportFailure{QStringLiteral("Import cancelled")};
            }
        };

        // The connection is held for the whole import, which is one transaction: a lookup made
        // meanwhile waits until it is done.
        QMutexLocker lock(&database.Mutex());
        SqliteDb& db = database.Db();
        {
            SqliteDb::Statement exists(db, "SELECT 1 FROM dictionaries WHERE title = ?");
            if (exists.Bind(1, title).Step()) {
                throw ImportFailure{QStringLiteral("\"%1\" is already installed").arg(title)};
            }
        }

        SqliteDb::Transaction transaction(db);
        int priority = 0;
        {
            SqliteDb::Statement next(db, "SELECT COALESCE(MAX(priority), -1) + 1 FROM dictionaries");
            if (next.Step()) {
                priority = static_cast<int>(next.Int(0));
            }
        }
        {
            SqliteDb::Statement insert(
                db, "INSERT INTO dictionaries(title, revision, author, description, "
                    "frequency_mode, priority) VALUES(?, ?, ?, ?, ?, ?)");
            insert.Bind(1, title)
                .Bind(2, index.value(QStringLiteral("revision")).toString())
                .Bind(3, index.value(QStringLiteral("author")).toString())
                .Bind(4, index.value(QStringLiteral("description")).toString())
                .Bind(5, index.value(QStringLiteral("frequencyMode")).toString())
                .Bind(6, priority)
                .Run();
        }
        const qint64 dictionary_id = db.LastInsertId();

        SqliteDb::Statement insert_term(
            db, "INSERT INTO terms(dict, expression, reading, def_tags, rules, score, glossary, "
                "sequence, term_tags) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
        SqliteDb::Statement insert_meta(
            db, "INSERT INTO term_meta(dict, expression, mode, data) VALUES(?, ?, ?, ?)");
        int done = 0;
        if (progress) {
            progress(done, total, title);
        }

        for (const QString& name : term_banks) {
            wanted();
            for (const QJsonValue& row_value : ParseBank(zip, name)) {
                const QJsonArray row = row_value.toArray();
                if (row.size() < 5) {
                    continue;
                }
                const QString expression = row.at(0).toString();
                const QString reading = row.at(1).toString();
                QString glossary;
                qint64 sequence = 0;
                QString term_tags;
                if (format >= 3) {
                    glossary = JsonText(row.at(5));
                    if (row.size() > 6) {
                        sequence = Number(row.at(6));
                    }
                    if (row.size() > 7) {
                        term_tags = Text(row.at(7));
                    }
                } else {
                    QJsonArray glosses;
                    for (qsizetype i = 5; i < row.size(); ++i) {
                        glosses.append(Text(row.at(i)));
                    }
                    glossary = JsonText(glosses);
                }
                insert_term.Bind(1, dictionary_id)
                    .Bind(2, expression)
                    // An empty reading means the expression is its own reading.
                    .Bind(3, reading == expression ? QString() : reading)
                    .Bind(4, Text(row.at(2)))
                    .Bind(5, Text(row.at(3)))
                    .Bind(6, Number(row.at(4)))
                    .BindBlob(7, DictionaryDatabase::Pack(glossary))
                    .Bind(8, sequence)
                    .Bind(9, term_tags)
                    .Run();
                ++result.terms;
            }
            if (progress) {
                progress(++done, total, title);
            }
        }

        for (const QString& name : meta_banks) {
            wanted();
            for (const QJsonValue& row_value : ParseBank(zip, name)) {
                const QJsonArray row = row_value.toArray();
                if (row.size() < 3) {
                    continue;
                }
                insert_meta.Bind(1, dictionary_id)
                    .Bind(2, row.at(0).toString())
                    .Bind(3, row.at(1).toString())
                    .Bind(4, JsonText(row.at(2)))
                    .Run();
                ++result.metas;
            }
            if (progress) {
                progress(++done, total, title);
            }
        }

        SqliteDb::Statement counts(db, "UPDATE dictionaries SET terms = ?, metas = ? WHERE id = ?");
        counts.Bind(1, result.terms).Bind(2, result.metas).Bind(3, dictionary_id).Run();
        transaction.Commit();
        result.ok = true;
    } catch (const ImportFailure& failure) {
        result.error = failure.message;
    } catch (const SqliteError& error) {
        result.error = result.title.isEmpty()
                           ? QString::fromUtf8(error.what())
                           : QStringLiteral("\"%1\" could not be imported: %2")
                                 .arg(result.title, QString::fromUtf8(error.what()));
    }
    if (!result.ok) {
        result.terms = 0;
        result.metas = 0;
    }
    return result;
}

} // namespace ReaderApp
