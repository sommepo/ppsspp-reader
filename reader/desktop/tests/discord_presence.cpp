// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The Discord status against a stand-in for the Discord program: a local socket of the test's
// own that speaks Discord's side of the pipe. Made-up names; no Discord is looked for.

#include <functional>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include "reader/desktop/discord_presence.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

/// Sockets and timers need an application object and its events run.
void NeedApplication() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "reader_desktop_tests";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);
    }
}

bool Until(const std::function<bool()>& done, int within_ms = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > within_ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

void Run(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

/// Discord's side: takes the handshake, says READY, keeps every command.
struct StandIn {
    explicit StandIn(const QString& name) {
        QLocalServer::removeServer(name);
        REQUIRE(server.listen(name));
        QObject::connect(&server, &QLocalServer::newConnection, [this] {
            client = server.nextPendingConnection();
            ++connections;
            QObject::connect(client, &QLocalSocket::readyRead, [this] {
                buffer += client->readAll();
                while (buffer.size() >= 8) {
                    const quint32 opcode = qFromLittleEndian<quint32>(buffer.constData());
                    const quint32 length = qFromLittleEndian<quint32>(buffer.constData() + 4);
                    if (buffer.size() < 8 + static_cast<qsizetype>(length)) {
                        return;
                    }
                    const QJsonObject message =
                        QJsonDocument::fromJson(buffer.mid(8, static_cast<qsizetype>(length)))
                            .object();
                    buffer.remove(0, 8 + static_cast<qsizetype>(length));
                    if (opcode == 0) {
                        handshake = message;
                        if (refuse) {
                            client->write(DiscordPresence::Frame(
                                2, QJsonObject{{QStringLiteral("code"), 4000},
                                               {QStringLiteral("message"),
                                                QStringLiteral("Invalid Client ID")}}));
                            client->flush();
                            client->disconnectFromServer();
                        } else {
                            client->write(DiscordPresence::Frame(
                                1, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("DISPATCH")},
                                               {QStringLiteral("evt"), QStringLiteral("READY")}}));
                        }
                    } else if (opcode == 1) {
                        commands.append(message);
                    } else if (opcode == 4) {
                        pongs.append(message);
                    }
                }
            });
        });
    }

    [[nodiscard]] QJsonObject LastActivity() const {
        return commands.last()
            .value(QStringLiteral("args"))
            .toObject()
            .value(QStringLiteral("activity"))
            .toObject();
    }

    QLocalServer server;
    QLocalSocket* client{};
    QByteArray buffer;
    QJsonObject handshake;
    QList<QJsonObject> commands;
    QList<QJsonObject> pongs;
    int connections{};
    bool refuse{};
};

const QString AppId = QStringLiteral("123456789012345678");

DiscordPresence::Activity Reading(qint64 characters, qint64 active_ms) {
    DiscordPresence::Activity activity;
    activity.game = Q(u"作り物の物語");
    activity.characters = characters;
    activity.active_ms = active_ms;
    activity.started_s = 1791300000;
    return activity;
}

} // namespace

TEST_CASE("DiscordPresence: what the status says", "[reader][discord]") {
    // No speed from less than a minute of reading: it would jump about.
    CHECK(DiscordPresence::StateText(Reading(120, 30000)) == Q(u"120字"));
    CHECK(DiscordPresence::StateText(Reading(1234, 600000)) == Q(u"1,234字 · 7,404字/時"));
    CHECK(DiscordPresence::StateText(Reading(0, 600000)) == Q(u"0字"));
    DiscordPresence::Activity paused = Reading(1234, 600000);
    paused.paused = true;
    CHECK(DiscordPresence::StateText(paused) == Q(u"1,234字 · 7,404字/時 · paused"));

    const QJsonObject json = DiscordPresence::ActivityJson(Reading(1234, 600000));
    CHECK(json.value(QStringLiteral("details")).toString() == Q(u"作り物の物語"));
    CHECK(json.value(QStringLiteral("state")).toString() == Q(u"1,234字 · 7,404字/時"));
    CHECK(json.value(QStringLiteral("timestamps")).toObject().value(QStringLiteral("start")).toInteger() ==
          1791300000);
    CHECK(json.value(QStringLiteral("type")).toInt(-1) == 0);

    // Discord takes texts of 2 to 128 characters.
    DiscordPresence::Activity odd = Reading(5, 0);
    odd.game = QStringLiteral("X");
    CHECK(DiscordPresence::ActivityJson(odd).value(QStringLiteral("details")).toString().size() == 2);
    odd.game = QString(300, QLatin1Char('x'));
    CHECK(DiscordPresence::ActivityJson(odd).value(QStringLiteral("details")).toString().size() == 128);
    odd.started_s = 0;
    CHECK_FALSE(DiscordPresence::ActivityJson(odd).contains(QStringLiteral("timestamps")));
}

TEST_CASE("DiscordPresence: an application ID is a long number", "[reader][discord]") {
    CHECK(DiscordPresence::IsApplicationId(AppId));
    CHECK(DiscordPresence::IsApplicationId(QStringLiteral("12345678901234567890")));
    CHECK_FALSE(DiscordPresence::IsApplicationId(QString()));
    CHECK_FALSE(DiscordPresence::IsApplicationId(QStringLiteral("1234")));
    CHECK_FALSE(DiscordPresence::IsApplicationId(QStringLiteral("12345678901234567x")));
    CHECK_FALSE(DiscordPresence::IsApplicationId(QStringLiteral("123456789012345678901")));
}

TEST_CASE("DiscordPresence: a message as it travels", "[reader][discord]") {
    const QByteArray frame = DiscordPresence::Frame(1, QJsonObject{{QStringLiteral("a"), 1}});
    REQUIRE(frame.size() == 8 + 7);
    CHECK(frame.left(8) == QByteArray("\x01\x00\x00\x00\x07\x00\x00\x00", 8));
    CHECK(frame.mid(8) == QByteArray("{\"a\":1}"));
}

TEST_CASE("DiscordPresence: the status goes to Discord, changes with the reading and is taken back",
          "[reader][discord]") {
    NeedApplication();
    QTemporaryDir folder;
    const QString pipe = QDir(folder.path()).filePath(QStringLiteral("discord-ipc-0"));
    StandIn discord{pipe};

    DiscordPresence presence;
    // The first place it looks has nobody: it goes on to the next.
    presence.SetPipes({QDir(folder.path()).filePath(QStringLiteral("nobody")), pipe});
    presence.SetPace(150, 100);
    presence.Configure(true, AppId);
    // Nothing to show: Discord is not even looked for.
    Run(100);
    CHECK(discord.connections == 0);

    presence.Set(Reading(120, 30000));
    REQUIRE(Until([&] { return discord.commands.size() == 1; }));
    CHECK(presence.IsReady());
    CHECK(discord.handshake.value(QStringLiteral("v")).toInt() == 1);
    CHECK(discord.handshake.value(QStringLiteral("client_id")).toString() == AppId);
    const QJsonObject first = discord.commands.first();
    CHECK(first.value(QStringLiteral("cmd")).toString() == QStringLiteral("SET_ACTIVITY"));
    CHECK(first.value(QStringLiteral("args")).toObject().value(QStringLiteral("pid")).toInteger() ==
          QCoreApplication::applicationPid());
    CHECK_FALSE(first.value(QStringLiteral("nonce")).toString().isEmpty());
    CHECK(discord.LastActivity().value(QStringLiteral("state")).toString() == Q(u"120字"));

    // Three changes in quick succession are one message, with the last of them, and not
    // before the pause Discord asks for is over.
    QElapsedTimer since;
    since.start();
    presence.Set(Reading(150, 40000));
    presence.Set(Reading(180, 50000));
    presence.Set(Reading(1234, 600000));
    REQUIRE(Until([&] { return discord.commands.size() == 2; }));
    CHECK(since.elapsed() >= 100);
    CHECK(discord.LastActivity().value(QStringLiteral("state")).toString() ==
          Q(u"1,234字 · 7,404字/時"));
    Run(300);
    CHECK(discord.commands.size() == 2);
    // The same again is not sent again.
    presence.Set(Reading(1234, 600000));
    Run(300);
    CHECK(discord.commands.size() == 2);

    // Discord asks whether the program is still there.
    discord.client->write(DiscordPresence::Frame(3, QJsonObject{{QStringLiteral("n"), 7}}));
    REQUIRE(Until([&] { return discord.pongs.size() == 1; }));
    CHECK(discord.pongs.first().value(QStringLiteral("n")).toInt() == 7);

    // The game stops: the status is taken back.
    presence.Set(std::nullopt);
    REQUIRE(Until([&] { return discord.commands.size() == 3; }));
    CHECK(discord.commands.last().value(QStringLiteral("args")).toObject().value(QStringLiteral("activity")).isNull());
    CHECK(discord.commands.last().value(QStringLiteral("args")).toObject().contains(QStringLiteral("activity")));

    // Switched off: the connection goes, and nothing more is said.
    presence.Set(Reading(2000, 700000));
    REQUIRE(Until([&] { return discord.commands.size() == 4; }));
    presence.Configure(false, AppId);
    CHECK_FALSE(presence.IsReady());
    presence.Set(Reading(3000, 800000));
    Run(300);
    CHECK(discord.commands.size() == 4);
    CHECK(presence.Status() == QStringLiteral("Off."));
}

TEST_CASE("DiscordPresence: Discord started later, or restarted, gets the status then",
          "[reader][discord]") {
    NeedApplication();
    QTemporaryDir folder;
    const QString pipe = QDir(folder.path()).filePath(QStringLiteral("discord-ipc-0"));

    DiscordPresence presence;
    presence.SetPipes({pipe});
    presence.SetPace(50, 100);
    presence.Configure(true, AppId);
    presence.Set(Reading(500, 120000));
    Run(250);
    CHECK_FALSE(presence.IsReady());
    CHECK(presence.Status().startsWith(QStringLiteral("Discord is not running")));

    {
        StandIn discord{pipe};
        REQUIRE(Until([&] { return discord.commands.size() == 1; }));
        CHECK(discord.LastActivity().value(QStringLiteral("state")).toString() ==
              Q(u"500字 · 15,000字/時"));
        CHECK(presence.Status() == QStringLiteral("Connected to Discord."));
        discord.client->abort();
        REQUIRE(Until([&] { return !presence.IsReady(); }));
    }
    // And a new one is told what is current by then.
    presence.Set(Reading(900, 180000));
    StandIn again{pipe};
    REQUIRE(Until([&] { return again.commands.size() == 1; }));
    CHECK(again.LastActivity().value(QStringLiteral("state")).toString() ==
          Q(u"900字 · 18,000字/時"));
}

TEST_CASE("DiscordPresence: without a proper application ID nothing is tried", "[reader][discord]") {
    NeedApplication();
    QTemporaryDir folder;
    const QString pipe = QDir(folder.path()).filePath(QStringLiteral("discord-ipc-0"));
    StandIn discord{pipe};

    DiscordPresence presence;
    presence.SetPipes({pipe});
    presence.SetPace(50, 100);
    presence.Configure(true, QStringLiteral("my app"));
    presence.Set(Reading(500, 120000));
    Run(250);
    CHECK(discord.connections == 0);
    CHECK(presence.Status().startsWith(QStringLiteral("No application ID")));

    // An ID Discord does not know: it closes, and is asked again later, not in a tight loop.
    discord.refuse = true;
    presence.Configure(true, AppId);
    REQUIRE(Until([&] { return discord.connections >= 1; }));
    Run(350);
    CHECK_FALSE(presence.IsReady());
    CHECK(discord.connections <= 5);
    CHECK(discord.commands.isEmpty());
}
