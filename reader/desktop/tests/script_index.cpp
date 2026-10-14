// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PCSX2 Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "reader/desktop/script_index.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;
using namespace ReaderApp::Scripts;

namespace {

const QByteArray MadeUp = "-- reader: no hooks\n-- reader: title 作り話\n"
                          "-- reader: reads The message window\n"
                          "-- reader: version ULJM99999\nlocal x = 1\n";
const QByteArray MadeUpNewer = MadeUp + "local y = 2\n";
const QByteArray Table = "あいうえお\n";

QJsonObject FileJson(const QString& name, const QByteArray& bytes) {
    return {{QStringLiteral("file"), name},
            {QStringLiteral("sha256"), Sha256(bytes)},
            {QStringLiteral("size"), bytes.size()}};
}

QByteArray IndexJson(const QByteArray& script, bool with_table = false, int api = 1) {
    QJsonObject entry = FileJson(QStringLiteral("ULJM99999.lua"), script);
    entry.insert(QStringLiteral("id"), QStringLiteral("ULJM99999"));
    entry.insert(QStringLiteral("title"), Q(u"作り話"));
    entry.insert(QStringLiteral("reads"), QStringLiteral("The message window"));
    entry.insert(QStringLiteral("version"), QStringLiteral("ULJM99999"));
    entry.insert(QStringLiteral("api"), api);
    if (with_table) {
        entry.insert(QStringLiteral("data"),
                     QJsonArray{FileJson(QStringLiteral("ULJM99999.charset"), Table)});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), 1},
                                     {QStringLiteral("system"), QStringLiteral("psp")},
                                     {QStringLiteral("scripts"), QJsonArray{entry}}})
        .toJson();
}

Entry EntryOf(const QByteArray& json) {
    const auto index = ParseIndex(json);
    REQUIRE(index.has_value());
    REQUIRE(index->entries.size() == 1);
    return index->entries.first();
}

void WriteTo(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

TEST_CASE("Scripts: the published list is read", "[reader][scripts]") {
    const Entry entry = EntryOf(IndexJson(MadeUp, true));
    REQUIRE(entry.id == QStringLiteral("ULJM99999"));
    REQUIRE(entry.title == Q(u"作り話"));
    REQUIRE(entry.script.file == QStringLiteral("ULJM99999.lua"));
    REQUIRE(entry.script.sha256 == Sha256(MadeUp));
    REQUIRE(entry.script.size == MadeUp.size());
    REQUIRE(entry.data.size() == 1);
    REQUIRE(entry.data.first().file == QStringLiteral("ULJM99999.charset"));

    QString error;
    REQUIRE_FALSE(ParseIndex("not json", &error).has_value());
    REQUIRE_FALSE(error.isEmpty());
    // A format from a later app is refused, not half read.
    REQUIRE_FALSE(ParseIndex(R"({"format": 2, "scripts": []})", &error).has_value());

    // An entry whose file is not its game's script, or a path, is passed over.
    QJsonObject root = QJsonDocument::fromJson(IndexJson(MadeUp)).object();
    QJsonArray scripts = root.value(QStringLiteral("scripts")).toArray();
    QJsonObject wrong = scripts.first().toObject();
    wrong.insert(QStringLiteral("file"), QStringLiteral("../ULJM99999.lua"));
    QJsonObject other = scripts.first().toObject();
    other.insert(QStringLiteral("id"), QStringLiteral("ULJM99998"));
    root.insert(QStringLiteral("scripts"), QJsonArray{wrong, other});
    const auto index = ParseIndex(QJsonDocument(root).toJson());
    REQUIRE(index.has_value());
    REQUIRE(index->entries.isEmpty());
}

TEST_CASE("Scripts: which files a script may be or ask for", "[reader][scripts]") {
    REQUIRE(IdOfFile(QStringLiteral("ULJS00219.lua")) == QStringLiteral("ULJS00219"));
    REQUIRE(IdOfFile(QStringLiteral("ULJS00219.charset")) == QStringLiteral("ULJS00219"));
    REQUIRE(IdOfFile(QStringLiteral("ULJS00219.table.bin")) == QStringLiteral("ULJS00219"));
    REQUIRE_FALSE(IdOfFile(QStringLiteral(".installed.json")).has_value());
    REQUIRE_FALSE(IdOfFile(QStringLiteral("ULJS00219..lua")).has_value());
    REQUIRE_FALSE(IdOfFile(QStringLiteral("../ULJS00219.lua")).has_value());
    REQUIRE_FALSE(IdOfFile(QStringLiteral("ULJS00219")).has_value());
    REQUIRE_FALSE(IdOfFile(QStringLiteral("notes.txt")).has_value());
}

TEST_CASE("Scripts: the API a script needs", "[reader][scripts]") {
    REQUIRE(ApiOf(QString::fromUtf8(MadeUp)) == 1);
    REQUIRE(ApiOf(QStringLiteral("-- reader: title x\n-- reader: api 3\nlocal x\n")) == 3);
    // Only the comments at the top say anything.
    REQUIRE(ApiOf(QStringLiteral("local x\n-- reader: api 3\n")) == 1);
}

TEST_CASE("Scripts: how the list's script compares with what the app has", "[reader][scripts]") {
    const Entry entry = EntryOf(IndexJson(MadeUpNewer));
    const QString old_sha = Sha256(MadeUp);
    REQUIRE(StateOf(entry, std::nullopt, std::nullopt, {}) == State::NotInstalled);
    REQUIRE(StateOf(entry, MadeUpNewer, std::nullopt, {}) == State::Installed);
    REQUIRE(StateOf(entry, std::nullopt, MadeUpNewer, {}) == State::Installed);
    // The app comes with an older one; one installed from the list earlier, unchanged.
    REQUIRE(StateOf(entry, std::nullopt, MadeUp, {}) == State::Update);
    REQUIRE(StateOf(entry, MadeUp, std::nullopt, old_sha) == State::Update);
    // The user's own, or one of the list's that the user changed.
    REQUIRE(StateOf(entry, MadeUp, std::nullopt, {}) == State::Own);
    REQUIRE(StateOf(entry, MadeUp + "-- mine\n", std::nullopt, old_sha) == State::Own);
    REQUIRE(StateOf(entry, MadeUp + "-- mine\n", MadeUpNewer, {}) == State::Own);
    REQUIRE(StateOf(EntryOf(IndexJson(MadeUp, false, Api + 1)), std::nullopt, std::nullopt, {}) ==
            State::TooNew);
}

TEST_CASE("Scripts: installing from the list, updating, removing", "[reader][scripts]") {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/profiles"); // made when first needed
    Folder folder(path);
    const Entry entry = EntryOf(IndexJson(MadeUp, true));
    QString error;

    // A file that is not the one the list names is refused, and nothing is written.
    REQUIRE_FALSE(folder.Install(entry,
                                 {{QStringLiteral("ULJM99999.lua"), MadeUpNewer},
                                  {QStringLiteral("ULJM99999.charset"), Table}},
                                 &error));
    REQUIRE_FALSE(error.isEmpty());
    REQUIRE_FALSE(folder.Script(QStringLiteral("ULJM99999")).has_value());
    // So is a list without the data file fetched.
    REQUIRE_FALSE(folder.Install(entry, {{QStringLiteral("ULJM99999.lua"), MadeUp}}, &error));

    REQUIRE(folder.Install(entry,
                           {{QStringLiteral("ULJM99999.lua"), MadeUp},
                            {QStringLiteral("ULJM99999.charset"), Table}},
                           &error));
    REQUIRE(folder.Script(QStringLiteral("ULJM99999")) == MadeUp);
    REQUIRE(QFile::exists(path + QStringLiteral("/ULJM99999.charset")));
    REQUIRE(folder.Recorded(QStringLiteral("ULJM99999")) == Sha256(MadeUp));
    REQUIRE(StateOf(entry, folder.Script(entry.id), std::nullopt, folder.Recorded(entry.id)) ==
            State::Installed);

    // The list moves on: an update, and installing it.
    const Entry newer = EntryOf(IndexJson(MadeUpNewer));
    REQUIRE(StateOf(newer, folder.Script(entry.id), std::nullopt, folder.Recorded(entry.id)) ==
            State::Update);
    REQUIRE(folder.Install(newer, {{QStringLiteral("ULJM99999.lua"), MadeUpNewer}}, &error));
    REQUIRE(folder.Script(entry.id) == MadeUpNewer);

    // Removing takes the script and what came with it from the list.
    REQUIRE(folder.Install(entry,
                           {{QStringLiteral("ULJM99999.lua"), MadeUp},
                            {QStringLiteral("ULJM99999.charset"), Table}},
                           &error));
    REQUIRE(folder.Remove(entry.id));
    REQUIRE_FALSE(folder.Script(entry.id).has_value());
    REQUIRE_FALSE(QFile::exists(path + QStringLiteral("/ULJM99999.charset")));
    REQUIRE(folder.Recorded(entry.id).isEmpty());
    REQUIRE_FALSE(folder.Remove(entry.id));
}

TEST_CASE("Scripts: installing files the user chose", "[reader][scripts]") {
    QTemporaryDir source;
    QTemporaryDir dir;
    Folder folder(dir.path());
    const QString script = source.path() + QStringLiteral("/ULJM99999.lua");
    const QString table = source.path() + QStringLiteral("/ULJM99999.charset");
    const QString stray = source.path() + QStringLiteral("/my script.lua");
    const QString binary = source.path() + QStringLiteral("/ULJM99998.lua");
    WriteTo(script, MadeUp);
    WriteTo(table, Table);
    WriteTo(stray, MadeUp);
    WriteTo(binary, QByteArray("\xff\xfe\x00\x01", 4));

    // One that came from the list first: choosing a file makes it the user's own.
    const Entry entry = EntryOf(IndexJson(MadeUpNewer));
    QString error;
    REQUIRE(folder.Install(entry, {{QStringLiteral("ULJM99999.lua"), MadeUpNewer}}, &error));

    const QStringList installed = folder.InstallFiles({script, table, stray, binary}, &error);
    REQUIRE(installed == QStringList{QStringLiteral("ULJM99999")});
    REQUIRE(error.contains(QStringLiteral("my script.lua")));
    REQUIRE(error.contains(QStringLiteral("ULJM99998.lua")));
    REQUIRE(folder.Script(QStringLiteral("ULJM99999")) == MadeUp);
    REQUIRE(QFile::exists(dir.path() + QStringLiteral("/ULJM99999.charset")));
    REQUIRE_FALSE(QFile::exists(dir.path() + QStringLiteral("/ULJM99998.lua")));
    REQUIRE(folder.Recorded(QStringLiteral("ULJM99999")).isEmpty());
    REQUIRE(StateOf(entry, folder.Script(entry.id), std::nullopt, folder.Recorded(entry.id)) ==
            State::Own);
}
