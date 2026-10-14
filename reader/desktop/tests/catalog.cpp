// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "reader/desktop/catalog.h"
#include "reader/desktop/settings.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

TEST_CASE("Catalog: a profile says what it is in its first lines", "[reader][catalog]") {
    const QString unknown = QStringLiteral("ULJM09999");
    const ProfileInfo info = Catalog::Describe(unknown, Q(uR"(
-- reader: no hooks
-- reader: title 作り話 (A Made-up Game)
--   reader:   reads   Dialogue and choices
-- reader: version Base game, version 1
-- reader: something a later reader may know
-- An ordinary comment about how the game keeps its text.
local window = 0x1234
-- reader: title Not this: the comments at the top have ended
)"),
                                               false);
    REQUIRE(info.title == Q(u"作り話 (A Made-up Game)"));
    REQUIRE(info.reads == QStringLiteral("Dialogue and choices"));
    REQUIRE(info.version == QStringLiteral("Base game, version 1"));
    REQUIRE_FALSE(info.own);
    // A profile that says nothing is known by its ID.
    const ProfileInfo silent =
        Catalog::Describe(unknown, QStringLiteral("-- A profile of my own.\nlocal x = 1\n"), true);
    REQUIRE(silent.title == unknown);
    REQUIRE(silent.reads.isEmpty());
    REQUIRE(silent.version.isEmpty());
    REQUIRE(silent.own);
}

TEST_CASE("Catalog: which files are profiles", "[reader][catalog]") {
    REQUIRE(Catalog::TitleIdOf(QStringLiteral("ULJS00219.lua")) ==
            QStringLiteral("ULJS00219"));
    REQUIRE_FALSE(Catalog::TitleIdOf(QStringLiteral("ULJS00219.table.bin")).has_value());
    REQUIRE_FALSE(Catalog::TitleIdOf(QStringLiteral("notes.lua")).has_value());
    REQUIRE_FALSE(Catalog::TitleIdOf(QStringLiteral("0100d7f00fb1a000.lua")).has_value());
}

TEST_CASE("Catalog: every bundled profile says its game, what is read and for which version",
          "[reader][catalog]") {
    const QDir folder(QStringLiteral(READER_BUNDLED_PROFILE_DIR));
    QStringList ids;
    QStringList without_voices;
    for (const QString& name : folder.entryList(QDir::Files)) {
        const auto id = Catalog::TitleIdOf(name);
        if (!id) {
            continue;
        }
        ids.append(*id);
        QFile file(folder.filePath(name));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const ProfileInfo info =
            Catalog::Describe(*id, QString::fromUtf8(file.read(4096)), false);
        INFO(id->toStdString());
        REQUIRE(info.title != *id);
        REQUIRE_FALSE(info.title.trimmed().isEmpty());
        REQUIRE_FALSE(info.reads.trimmed().isEmpty());
        REQUIRE_FALSE(info.version.trimmed().isEmpty());
        // A profile that speaks of the audio says one of the three choices.
        if (!info.audio.isEmpty()) {
            REQUIRE(ReaderSettings::CardAudioOfValue(info.audio).has_value());
            if (info.audio == QStringLiteral("never")) {
                without_voices.append(*id);
            }
        }
    }
    // The games whose lines have no voices: 428. Their cards get no audio unless the user
    // says otherwise.
    std::sort(without_voices.begin(), without_voices.end());
    REQUIRE(without_voices == QStringList{QStringLiteral("ULJS00219")});
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids == QStringList{QStringLiteral("ULJM05718"), QStringLiteral("ULJM05719"),
                               QStringLiteral("ULJM05821"), QStringLiteral("ULJM05887"),
                               QStringLiteral("ULJS00219")});
    // The two discs of D.C.I&II P.S.P. are one program built twice: their profiles are the
    // same after the lines that name the disc.
    const auto body = [&folder](const char* name) {
        QFile file(folder.filePath(QString::fromLatin1(name)));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const QByteArray all = file.readAll();
        const qsizetype at = all.indexOf("-- The two discs are one program");
        REQUIRE(at > 0);
        return all.mid(at);
    };
    REQUIRE(body("ULJM05718.lua") == body("ULJM05719.lua"));
}

TEST_CASE("Settings: where the saves are kept is a choice, and an older setup is kept",
          "[reader][settings]") {
    QTemporaryDir folder;
    ReaderSettings settings(folder.filePath(QStringLiteral("reader.ini")));
    using Method = ReaderSettings::SaveSyncMethod;
    // Nothing is preset, and the question of the first start has not been asked.
    REQUIRE(settings.SaveSyncMode() == Method::Off);
    REQUIRE_FALSE(settings.SaveSyncEnabled());
    REQUIRE_FALSE(settings.SaveSyncAsked());
    REQUIRE(settings.SaveSyncPort() == 8642);
    // A setup from before there was a choice: the switch alone means another computer.
    settings.SetValue(ReaderSettings::KeySaveSyncEnabled, true);
    REQUIRE(settings.SaveSyncMode() == Method::Client);
    REQUIRE(settings.SaveSyncEnabled());
    // The choice comes first once it is made.
    settings.SetValue(ReaderSettings::KeySaveSyncMode, QStringLiteral("hub"));
    REQUIRE(settings.SaveSyncMode() == Method::Hub);
    settings.SetValue(ReaderSettings::KeySaveSyncMode, QStringLiteral("off"));
    REQUIRE(settings.SaveSyncMode() == Method::Off);
    REQUIRE_FALSE(settings.SaveSyncEnabled());
    settings.SetValue(ReaderSettings::KeySaveSyncMode, QStringLiteral(" Client "));
    REQUIRE(settings.SaveSyncMode() == Method::Client);
    for (const Method method : {Method::Off, Method::Hub, Method::Client}) {
        settings.SetValue(ReaderSettings::KeySaveSyncMode, ReaderSettings::SaveSyncModeText(method));
        REQUIRE(settings.SaveSyncMode() == method);
    }
    // A port a program may not listen on is not taken.
    settings.SetValue(ReaderSettings::KeySaveSyncPort, 80);
    REQUIRE(settings.SaveSyncPort() == 1024);
    settings.SetValue(ReaderSettings::KeySaveSyncPort, 18642);
    REQUIRE(settings.SaveSyncPort() == 18642);
}

TEST_CASE("Settings: the line's audio on a card is a choice, for all games and per game",
          "[reader][settings]") {
    QTemporaryDir folder;
    ReaderSettings settings(folder.filePath(QStringLiteral("reader.ini")));
    using Audio = ReaderSettings::CardAudio;
    const QString game = QStringLiteral("ULJS00219");
    const QString other = QStringLiteral("ULJM05000");
    // Nothing is preset: every card asks.
    REQUIRE(settings.CardAudioGeneral() == Audio::Ask);
    REQUIRE_FALSE(settings.CardAudioOf(game).has_value());
    REQUIRE(settings.CardAudioFor(game) == Audio::Ask);
    // A game's own choice comes first; the others follow the general one.
    settings.SetCardAudioOf(game, Audio::Never);
    settings.SetValue(ReaderSettings::KeyCardAudio, QStringLiteral("always"));
    REQUIRE(settings.CardAudioFor(game) == Audio::Never);
    REQUIRE(settings.CardAudioFor(other) == Audio::Always);
    REQUIRE(settings.CardAudioFor(QString()) == Audio::Always);
    // Taken away, the game follows the general one again.
    settings.SetCardAudioOf(game, std::nullopt);
    REQUIRE_FALSE(settings.CardAudioOf(game).has_value());
    REQUIRE(settings.CardAudioFor(game) == Audio::Always);
    // What the game's profile says stands between the two: before the general choice, after
    // the user's own for the game.
    REQUIRE(settings.CardAudioFor(game, Audio::Never) == Audio::Never);
    settings.SetCardAudioOf(game, Audio::Ask);
    REQUIRE(settings.CardAudioFor(game, Audio::Never) == Audio::Ask);
    settings.SetCardAudioOf(game, std::nullopt);
    REQUIRE(Catalog::Describe(game, QStringLiteral("-- reader: audio Never\n"), false).audio ==
            QStringLiteral("never"));
    // What is not one of the three is no choice.
    settings.SetValue(ReaderSettings::KeyCardAudio, QStringLiteral("sometimes"));
    REQUIRE(settings.CardAudioGeneral() == Audio::Ask);
    REQUIRE(ReaderSettings::CardAudioValue(Audio::Never) == QStringLiteral("never"));
}
