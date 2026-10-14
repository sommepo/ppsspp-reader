// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PCSX2 Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include "reader/desktop/catalog.h"
#include "reader/desktop/script_index.h"

namespace ReaderApp::Scripts {

namespace {

/// The list's own format; a later one is refused rather than half understood.
constexpr int IndexFormat = 1;
/// More than any script needs: a file larger than this is not taken.
constexpr qint64 ScriptMax = 1 << 20;
constexpr qint64 DataMax = 16 << 20;
const QString RecordName = QStringLiteral(".installed.json");

QString Text(const QJsonObject& object, const char* key) {
    return object.value(QLatin1StringView(key)).toString().trimmed();
}

std::optional<FileEntry> FileOf(const QJsonObject& object) {
    FileEntry file{Text(object, "file"), Text(object, "sha256").toLower(),
                   static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(-1))};
    if (!IdOfFile(file.file) || file.sha256.size() != 64 || file.size < 0) {
        return std::nullopt;
    }
    return file;
}

bool IsUtf8(const QByteArray& bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    (void)QString(decoder(bytes));
    return !decoder.hasError();
}

} // namespace

std::optional<QString> IdOfFile(const QString& name) {
    const qsizetype dot = name.indexOf(u'.');
    if (dot <= 0) {
        return std::nullopt;
    }
    static const QRegularExpression rest(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*")));
    const QString id = name.left(dot);
    if (!rest.match(name.mid(dot + 1)).hasMatch() ||
        Catalog::TitleIdOf(id + QStringLiteral(".lua")) != id) {
        return std::nullopt;
    }
    return id;
}

std::optional<Index> ParseIndex(const QByteArray& json, QString* error) {
    const auto fail = [error](const QString& why) -> std::optional<Index> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("The list of scripts is not JSON: %1").arg(parse.errorString()));
    }
    const QJsonObject root = document.object();
    const int format = root.value(QStringLiteral("format")).toInt(0);
    if (format < 1 || format > IndexFormat) {
        return fail(QStringLiteral("The list of scripts is in a format this app does not know "
                                   "(%1): a newer app may read it").arg(format));
    }
    Index index;
    index.system = Text(root, "system");
    for (const QJsonValue& value : root.value(QStringLiteral("scripts")).toArray()) {
        const QJsonObject object = value.toObject();
        Entry entry;
        entry.id = Text(object, "id");
        entry.title = Text(object, "title");
        entry.reads = Text(object, "reads");
        entry.version = Text(object, "version");
        entry.api = object.value(QStringLiteral("api")).toInt(1);
        const auto script = FileOf(object);
        // An entry that is not whole, or whose file is not its game's script, is passed over.
        if (!script || Catalog::TitleIdOf(script->file) != entry.id) {
            continue;
        }
        entry.script = *script;
        bool whole = true;
        for (const QJsonValue& data : object.value(QStringLiteral("data")).toArray()) {
            const auto file = FileOf(data.toObject());
            if (!file || IdOfFile(file->file) != entry.id || file->file == entry.script.file) {
                whole = false;
                break;
            }
            entry.data.append(*file);
        }
        if (!whole) {
            continue;
        }
        if (entry.title.isEmpty()) {
            entry.title = entry.id;
        }
        index.entries.append(entry);
    }
    return index;
}

QString Sha256(const QByteArray& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

int ApiOf(const QString& source) {
    for (const QString& raw : source.split(u'\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (!line.startsWith(QStringLiteral("--"))) {
            break; // as in Catalog::Describe: only the comments at the top say anything
        }
        static const QRegularExpression api(QStringLiteral("^--\\s*reader:\\s*api\\s+(\\d+)\\s*$"));
        const QRegularExpressionMatch match = api.match(line);
        if (match.hasMatch()) {
            return match.captured(1).toInt();
        }
    }
    return 1;
}

State StateOf(const Entry& entry, const std::optional<QByteArray>& user,
              const std::optional<QByteArray>& bundled, const QString& recorded) {
    if (entry.api > Api) {
        return State::TooNew;
    }
    if (user) {
        const QString sha = Sha256(*user);
        if (sha == entry.script.sha256) {
            return State::Installed;
        }
        // Installed from the list and not changed since: the list has moved on.
        return !recorded.isEmpty() && sha == recorded ? State::Update : State::Own;
    }
    if (bundled) {
        return Sha256(*bundled) == entry.script.sha256 ? State::Installed : State::Update;
    }
    return State::NotInstalled;
}

// ---- The user's folder ----

Folder::Folder(QString path_) : path(std::move(path_)) {}

std::optional<QByteArray> Folder::Script(const QString& id) const {
    QFile file(path + u'/' + id + QStringLiteral(".lua"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

QString Folder::Recorded(const QString& id) const {
    return ReadRecord().value(id);
}

QHash<QString, QString> Folder::ReadRecord() const {
    // {"<ID>": {"sha256": "...", "data": ["<ID>.charset", ...]}}: the data files are what
    // Remove takes away with the script.
    QHash<QString, QString> record;
    QFile file(path + u'/' + RecordName);
    if (!file.open(QIODevice::ReadOnly)) {
        return record;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        record.insert(it.key(), it.value().toObject().value(QStringLiteral("sha256")).toString());
    }
    return record;
}

bool Folder::WriteRecord(const QHash<QString, QString>& record) const {
    // The data files of each are kept as they were.
    QJsonObject old;
    {
        QFile file(path + u'/' + RecordName);
        if (file.open(QIODevice::ReadOnly)) {
            old = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    QJsonObject root;
    for (auto it = record.begin(); it != record.end(); ++it) {
        QJsonObject entry = old.value(it.key()).toObject();
        entry.insert(QStringLiteral("sha256"), it.value());
        root.insert(it.key(), entry);
    }
    QString unused;
    return WriteFile(RecordName, QJsonDocument(root).toJson(QJsonDocument::Indented), &unused);
}

bool Folder::WriteFile(const QString& name, const QByteArray& bytes, QString* error) const {
    if (!QDir().mkpath(path)) {
        *error = QStringLiteral("Cannot make the folder %1").arg(path);
        return false;
    }
    QSaveFile file(path + u'/' + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = QStringLiteral("Cannot write %1: %2").arg(name, file.errorString());
        return false;
    }
    return true;
}

bool Folder::Install(const Entry& entry, const QHash<QString, QByteArray>& files,
                     QString* error) {
    const auto check = [&files, &entry, error](const FileEntry& wanted) {
        const auto found = files.constFind(wanted.file);
        if (found == files.constEnd()) {
            *error = QStringLiteral("%1 was not fetched").arg(wanted.file);
            return false;
        }
        if (IdOfFile(wanted.file) != entry.id) {
            *error = QStringLiteral("%1 is not a file of %2").arg(wanted.file, entry.id);
            return false;
        }
        if (found->size() != wanted.size || Sha256(*found) != wanted.sha256) {
            *error = QStringLiteral("%1 is not the file the list names (it may have changed "
                                    "since the list was read: try again)").arg(wanted.file);
            return false;
        }
        return true;
    };
    if (entry.api > Api) {
        *error = QStringLiteral("This script needs a newer app");
        return false;
    }
    if (!check(entry.script)) {
        return false;
    }
    const QByteArray& source = files.value(entry.script.file);
    if (!IsUtf8(source) || ApiOf(QString::fromUtf8(source)) > Api) {
        *error = QStringLiteral("%1 is not a script this app can run").arg(entry.script.file);
        return false;
    }
    for (const FileEntry& data : entry.data) {
        if (!check(data)) {
            return false;
        }
    }
    // The data first: a script whose data is missing says so, data without its script is
    // never asked for.
    QJsonArray data_names;
    for (const FileEntry& data : entry.data) {
        if (!WriteFile(data.file, files.value(data.file), error)) {
            return false;
        }
        data_names.append(data.file);
    }
    if (!WriteFile(entry.script.file, source, error)) {
        return false;
    }
    QFile record_file(path + u'/' + RecordName);
    QJsonObject root;
    if (record_file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(record_file.readAll()).object();
        record_file.close();
    }
    root.insert(entry.id, QJsonObject{{QStringLiteral("sha256"), entry.script.sha256},
                                      {QStringLiteral("data"), data_names}});
    return WriteFile(RecordName, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

QStringList Folder::InstallFiles(const QStringList& paths, QString* error) {
    QStringList installed;
    QStringList problems;
    QHash<QString, QString> record = ReadRecord();
    for (const QString& file_path : paths) {
        const QString name = QFileInfo(file_path).fileName();
        const auto id = IdOfFile(name);
        if (!id) {
            problems.append(QStringLiteral("%1: not a game's script or data file (a script is "
                                           "named after its game, as SLPM-65555.lua)").arg(name));
            continue;
        }
        const bool script = name.endsWith(QStringLiteral(".lua"));
        QFile file(file_path);
        if (!file.open(QIODevice::ReadOnly)) {
            problems.append(QStringLiteral("%1: %2").arg(name, file.errorString()));
            continue;
        }
        if (file.size() > (script ? ScriptMax : DataMax)) {
            problems.append(QStringLiteral("%1: too large").arg(name));
            continue;
        }
        const QByteArray bytes = file.readAll();
        if (script && (!Catalog::TitleIdOf(name) || !IsUtf8(bytes))) {
            problems.append(QStringLiteral("%1: not a script (a script is UTF-8 text)").arg(name));
            continue;
        }
        if (script && ApiOf(QString::fromUtf8(bytes)) > Api) {
            problems.append(QStringLiteral("%1: needs a newer app").arg(name));
            continue;
        }
        QString why;
        if (!WriteFile(name, bytes, &why)) {
            problems.append(why);
            continue;
        }
        if (script) {
            installed.append(*id);
            record.remove(*id); // the user's own now
        }
    }
    WriteRecord(record);
    if (error) {
        *error = problems.join(u'\n');
    }
    return installed;
}

bool Folder::Remove(const QString& id) {
    QJsonObject root;
    {
        QFile file(path + u'/' + RecordName);
        if (file.open(QIODevice::ReadOnly)) {
            root = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    for (const QJsonValue& data : root.value(id).toObject().value(QStringLiteral("data")).toArray()) {
        const QString name = data.toString();
        if (IdOfFile(name) == id) {
            QFile::remove(path + u'/' + name);
        }
    }
    const bool had = QFile::remove(path + u'/' + id + QStringLiteral(".lua"));
    if (root.contains(id)) {
        root.remove(id);
        QString unused;
        WriteFile(RecordName, QJsonDocument(root).toJson(QJsonDocument::Indented), &unused);
    }
    return had;
}

} // namespace ReaderApp::Scripts
