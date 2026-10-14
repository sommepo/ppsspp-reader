// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include "reader/desktop/tests/test_util.h"
#include "reader/desktop/text_hook_server.h"

using namespace ReaderApp;

TEST_CASE("TextHookServer: the handshake's answer", "[reader][texthook]") {
    // The example of RFC 6455, section 1.3.
    REQUIRE(TextHookServer::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
            QByteArray("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST_CASE("TextHookServer: frames of every length", "[reader][texthook]") {
    const QByteArray small = TextHookServer::TextFrame("abc");
    REQUIRE(small == QByteArray("\x81\x03" "abc", 5));
    const QByteArray medium = TextHookServer::TextFrame(QByteArray(300, 'x'));
    REQUIRE(medium.size() == 304);
    REQUIRE(static_cast<quint8>(medium.at(0)) == 0x81);
    REQUIRE(static_cast<quint8>(medium.at(1)) == 126);
    REQUIRE(static_cast<quint8>(medium.at(2)) == 0x01);
    REQUIRE(static_cast<quint8>(medium.at(3)) == 0x2C);
    const QByteArray large = TextHookServer::TextFrame(QByteArray(70000, 'x'));
    REQUIRE(large.size() == 70010);
    REQUIRE(static_cast<quint8>(large.at(1)) == 127);
    REQUIRE(static_cast<quint8>(large.at(7)) == 0x01);
    REQUIRE(static_cast<quint8>(large.at(8)) == 0x11);
    REQUIRE(static_cast<quint8>(large.at(9)) == 0x70);
    REQUIRE(TextHookServer::Frame(0xA, {}) == QByteArray("\x8A\x00", 2));
}

TEST_CASE("TextHookServer: a line in each format", "[reader][texthook]") {
    TextHookServer server(0, false);
    server.game_id = QStringLiteral("ULJS00219");
    TextLine line;
    line.text = Q(u"異議あり!");
    line.speaker = Q(u"成歩堂");
    line.channel = QStringLiteral("dialogue");
    line.source_id = QStringLiteral("s:1");
    line.ruby = {{0, 2, Q(u"いぎ")}};
    line.timestamp = 1'700'000'000'123;

    REQUIRE(server.Encode(line) == Q(u"異議あり!"));
    server.include_speaker = true;
    REQUIRE(server.Encode(line) == Q(u"成歩堂\n異議あり!"));

    server.format = QStringLiteral("sentence");
    const QJsonObject sentence = QJsonDocument::fromJson(server.Encode(line).toUtf8()).object();
    REQUIRE(sentence.value(QStringLiteral("sentence")).toString() == Q(u"成歩堂\n異議あり!"));
    REQUIRE(sentence.size() == 1);

    server.format = QStringLiteral("json");
    const QJsonObject event = QJsonDocument::fromJson(server.Encode(line).toUtf8()).object();
    REQUIRE(event.value(QStringLiteral("format")).toString() ==
            QStringLiteral("azahar-reader.text-event"));
    REQUIRE(event.value(QStringLiteral("gameId")).toString() == QStringLiteral("ULJS00219"));
    REQUIRE(event.value(QStringLiteral("segmentId")).toString() == QStringLiteral("s:1"));
    REQUIRE(event.value(QStringLiteral("speaker")).toString() == Q(u"成歩堂"));
    REQUIRE(event.value(QStringLiteral("speakerIncluded")).toBool());
    REQUIRE(event.value(QStringLiteral("timestamp")).toString() ==
            QStringLiteral("2023-11-14T22:13:20.123Z"));
    // A line nobody says has no speaker to include.
    line.speaker.clear();
    REQUIRE_FALSE(QJsonDocument::fromJson(server.Encode(line).toUtf8())
                      .object()
                      .value(QStringLiteral("speakerIncluded"))
                      .toBool());
}
