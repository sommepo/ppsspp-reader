// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include "reader/desktop/anki.h"
#include "reader/desktop/settings.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

/// Answers one request the way AnkiConnect would, on a thread of its own, and keeps what was
/// asked. No Anki is involved.
class Stand : public QThread {
public:
    explicit Stand(QByteArray reply_) : reply{std::move(reply_)} {
        server.listen(QHostAddress::LocalHost, 0);
        port = server.serverPort();
        server.moveToThread(this);
        start();
    }
    ~Stand() override {
        wait();
    }

    int port{};
    QByteArray request;

protected:
    void run() override {
        if (!server.waitForNewConnection(5000)) {
            return;
        }
        QTcpSocket* socket = server.nextPendingConnection();
        while (!request.contains("\r\n\r\n") && socket->waitForReadyRead(5000)) {
            request += socket->readAll();
        }
        const qsizetype head = request.indexOf("\r\n\r\n") + 4;
        const qsizetype at = request.toLower().indexOf("content-length:");
        const qsizetype length =
            request.mid(at + 15, request.indexOf('\r', at) - at - 15).trimmed().toLongLong();
        while (request.size() < head + length && socket->waitForReadyRead(5000)) {
            request += socket->readAll();
        }
        socket->write(reply);
        socket->waitForBytesWritten(5000);
        socket->disconnectFromHost();
        delete socket;
        server.close();
    }

private:
    QTcpServer server;
    QByteArray reply;
};

QByteArray Ok(const QByteArray& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

/// One piece of a reply sent in chunks: its length in hexadecimal, then the piece.
QByteArray Chunk(const QByteArray& data) {
    return QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n";
}

QString Url(const Stand& stand) {
    return QStringLiteral("127.0.0.1:%1").arg(stand.port);
}

QJsonObject Sent(const Stand& stand) {
    return QJsonDocument::fromJson(stand.request.mid(stand.request.indexOf("\r\n\r\n") + 4))
        .object();
}

} // namespace

TEST_CASE("AnkiSearch: fields are matched exactly, whatever is in them", "[reader][anki]") {
    REQUIRE(AnkiSearch::FieldIs(QStringLiteral("Mining"), QStringLiteral("Word"), Q(u"食べる")) ==
            Q(u"\"deck:Mining\" \"Word:食べる\""));
    REQUIRE(AnkiSearch::FieldIs({}, QStringLiteral("My:Field"), QStringLiteral("a*b_c\"d\\e")) ==
            QStringLiteral("\"My\\:Field:a\\*b\\_c\\\"d\\\\e\""));
    REQUIRE(AnkiSearch::Notes({1, 22, 333}) == QStringLiteral("nid:1,22,333"));
}

TEST_CASE("AnkiConnectClient: a note is sent as AnkiConnect expects it", "[reader][anki]") {
    Stand stand(Ok(R"({"result": 1496198395707, "error": null})"));
    AnkiConnectClient client(Url(stand), QStringLiteral("secret"));
    const qint64 id = client.AddNote(QStringLiteral("Mining"), QStringLiteral("Basic"),
                                     {{QStringLiteral("Front"), Q(u"食べる")}},
                                     {QStringLiteral("ppsspp-reader")}, Duplicates::RefuseInDeck);
    REQUIRE(id == 1496198395707);
    stand.wait();
    REQUIRE(stand.request.startsWith("POST / HTTP/1.1\r\n"));
    const QJsonObject sent = Sent(stand);
    REQUIRE(sent.value(QStringLiteral("action")).toString() == QStringLiteral("addNote"));
    REQUIRE(sent.value(QStringLiteral("version")).toInt() == 6);
    REQUIRE(sent.value(QStringLiteral("key")).toString() == QStringLiteral("secret"));
    const QJsonObject note =
        sent.value(QStringLiteral("params")).toObject().value(QStringLiteral("note")).toObject();
    REQUIRE(note.value(QStringLiteral("deckName")).toString() == QStringLiteral("Mining"));
    REQUIRE(note.value(QStringLiteral("fields")).toObject().value(QStringLiteral("Front")).toString() ==
            Q(u"食べる"));
    const QJsonObject options = note.value(QStringLiteral("options")).toObject();
    REQUIRE_FALSE(options.value(QStringLiteral("allowDuplicate")).toBool());
    REQUIRE(options.value(QStringLiteral("duplicateScope")).toString() == QStringLiteral("deck"));
    REQUIRE(options.value(QStringLiteral("duplicateScopeOptions"))
                .toObject()
                .value(QStringLiteral("checkChildren"))
                .toBool());
}

TEST_CASE("AnkiConnectClient: replies in pieces, lists and media", "[reader][anki]") {
    {
        // A reply sent in chunks, as some servers do.
        Stand stand("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
                    Chunk("{\"result\": [\"De") + Chunk("fault\", \"Mining\"], \"e") +
                    Chunk("rror\": null}") + "0\r\n\r\n");
        AnkiConnectClient client(Url(stand));
        REQUIRE(client.DeckNames() ==
                QStringList{QStringLiteral("Default"), QStringLiteral("Mining")});
    }
    {
        Stand stand(Ok(R"({"result": "stored_1.jpg", "error": null})"));
        AnkiConnectClient client(Url(stand));
        REQUIRE(client.StoreMedia("\x01\x02\x03", QStringLiteral("a.jpg")) ==
                QStringLiteral("stored_1.jpg"));
        stand.wait();
        const QJsonObject params = Sent(stand).value(QStringLiteral("params")).toObject();
        REQUIRE(params.value(QStringLiteral("filename")).toString() == QStringLiteral("a.jpg"));
        REQUIRE(params.value(QStringLiteral("data")).toString() == QStringLiteral("AQID"));
    }
    {
        Stand stand(Ok(R"({"result": [5, 6], "error": null})"));
        AnkiConnectClient client(Url(stand));
        REQUIRE(client.FindNotes(QStringLiteral("D"), QStringLiteral("F"), QStringLiteral("v")) ==
                QList<qint64>{5, 6});
        stand.wait();
        REQUIRE(Sent(stand).value(QStringLiteral("params")).toObject().value(QStringLiteral("query")).toString() ==
                QStringLiteral("\"deck:D\" \"F:v\""));
    }
}

TEST_CASE("SourceAddress: a card through VN Library's media add-on gets the game's address",
          "[reader][anki]") {
    const QString url = SourceAddress::Of(QStringLiteral("ULJM05000"));
    REQUIRE(url == QStringLiteral("https://www.jp.playstation.com/software/title/uljm05000.html"));
    REQUIRE(SourceAddress::Of(QString()).isEmpty());
    REQUIRE(SourceAddress::Of(QStringLiteral("not a game")).isEmpty());
    const QString title = Q(u"ファタモルガーナの館 <R>");

    // Stands for Anki: straight, or behind the add-on, which refuses a note whose field of
    // that name holds no web address, in the add-on's own words.
    QString address_field;
    QList<QMap<QString, QString>> sent;
    const auto add = [&](const QMap<QString, QString>& fields) -> qint64 {
        sent.append(fields);
        if (fields.value(QStringLiteral("Expression")) == Q(u"重複")) {
            throw AnkiError(QStringLiteral("cannot create note because it is a duplicate"));
        }
        if (!address_field.isEmpty() && !SourceAddress::HasAddress(fields.value(address_field))) {
            throw AnkiError(
                fields.value(address_field).trimmed().isEmpty()
                    ? QStringLiteral("Map %1 to {url} in Yomitan so game media can be matched "
                                     "safely")
                          .arg(address_field)
                    : Q(u"The %1 field must contain Yomitan’s {url}, not the page title")
                          .arg(address_field));
        }
        return 1496198395707;
    };
    const auto note = [&](const QMap<QString, QString>& fields, const QString& known = {}) {
        return SourceAddress::AddNote(add, fields, url, title, known);
    };
    const QString expression = QStringLiteral("Expression");

    // Straight to AnkiConnect nothing is asked for and nothing is added to the note.
    const auto plain = note({{expression, Q(u"館")}, {QStringLiteral("source"), Q(u"館の話")}});
    REQUIRE(plain.field.isEmpty());
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.last().value(QStringLiteral("source")) == Q(u"館の話"));

    // Through the add-on: refused once, answered with the address around the field's text,
    // added. The user set nothing up for it.
    address_field = QStringLiteral("source");
    const auto first = note({{expression, Q(u"館")}, {QStringLiteral("source"), Q(u"館の話")}});
    REQUIRE(first.id == 1496198395707);
    REQUIRE(first.field == QStringLiteral("source"));
    REQUIRE(sent.size() == 3);
    REQUIRE(sent.last().value(QStringLiteral("source")) ==
            QStringLiteral("<a href=\"%1\">%2</a>").arg(url, Q(u"館の話")));
    REQUIRE(sent.last().value(expression) == Q(u"館"));

    // Remembered, the next card is right the first time.
    const auto second = note({{expression, Q(u"扉")}, {QStringLiteral("source"), Q(u"館の話")}},
                             QStringLiteral("source"));
    REQUIRE(second.field == QStringLiteral("source"));
    REQUIRE(sent.size() == 4);

    // A field nothing was mapped to gets the game's title, as text.
    address_field = QStringLiteral("Source");
    const auto unmapped = note({{expression, Q(u"鐘")}});
    REQUIRE(unmapped.field == QStringLiteral("Source"));
    REQUIRE(sent.last().value(QStringLiteral("Source")) ==
            QStringLiteral("<a href=\"%1\">%2</a>").arg(url, Q(u"ファタモルガーナの館 &lt;R&gt;")));

    // A format that maps the field to {url}, as in Yomitan, needs no help.
    qsizetype before = sent.size();
    const auto mapped = note({{expression, Q(u"鍵")}, {QStringLiteral("Source"), url}});
    REQUIRE(mapped.field.isEmpty());
    REQUIRE(sent.size() == before + 1);
    REQUIRE(sent.last().value(QStringLiteral("Source")) == url);

    // Anything else Anki says is passed on, and asked once.
    before = sent.size();
    try {
        note({{expression, Q(u"重複")}, {QStringLiteral("Source"), url}});
        FAIL("no error was raised");
    } catch (const AnkiError& error) {
        REQUIRE(error.Message() == QStringLiteral("cannot create note because it is a duplicate"));
    }
    REQUIRE(sent.size() == before + 1);

    // The add-on's refusals are told from other errors.
    REQUIRE(SourceAddress::AskedFor(QStringLiteral(
                "Map Source to {url} in Yomitan so game media can be matched safely")) ==
            QStringLiteral("Source"));
    REQUIRE(SourceAddress::AskedFor(
                Q(u"The Source URL field must contain Yomitan’s {url}, not the page title")) ==
            QStringLiteral("Source URL"));
    REQUIRE(SourceAddress::AskedFor(QStringLiteral(
                "Map the Picture field in Yomitan (leave its template blank)"))
                .isEmpty());
    REQUIRE(SourceAddress::AskedFor(QStringLiteral("Connection key missing or incorrect."))
                .isEmpty());
    // Without a game there is no address to give: the note is left as it is.
    const QMap<QString, QString> fields{{QStringLiteral("Source"), QStringLiteral("x")}};
    REQUIRE(SourceAddress::With(fields, QStringLiteral("Source"), {}, QStringLiteral("T")) == fields);
}

TEST_CASE("Settings: the field the add-on asked an address for is remembered per Anki address",
          "[reader][anki]") {
    QTemporaryDir folder;
    ReaderSettings settings(folder.filePath(QStringLiteral("reader.ini")));
    const QString bridge = QStringLiteral("https://z13.example.ts.net:8776");
    REQUIRE(settings.AnkiAddressField(bridge).isEmpty());
    settings.SetValue(ReaderSettings::KeyAnkiAddressField,
                      ReaderSettings::AddressFieldValue(bridge, QStringLiteral("Source URL")));
    REQUIRE(settings.AnkiAddressField(bridge) == QStringLiteral("Source URL"));
    // Cards that go another way are not given the address unasked.
    REQUIRE(settings.AnkiAddressField(QStringLiteral("http://127.0.0.1:8765")).isEmpty());
}

TEST_CASE("AnkiConnectClient: what goes wrong is said", "[reader][anki]") {
    {
        Stand stand(Ok(R"({"result": null, "error": "cannot create note because it is a duplicate"})"));
        AnkiConnectClient client(Url(stand));
        try {
            client.AddNote(QStringLiteral("D"), QStringLiteral("M"), {}, {}, Duplicates::Allow);
            FAIL("no error was raised");
        } catch (const AnkiError& error) {
            REQUIRE(error.Message() == QStringLiteral("cannot create note because it is a duplicate"));
        }
    }
    {
        Stand stand("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
        AnkiConnectClient client(Url(stand));
        REQUIRE_THROWS_AS(client.Version(), AnkiError);
    }
    {
        Stand stand(Ok("this is not json"));
        AnkiConnectClient client(Url(stand));
        REQUIRE_THROWS_AS(client.Version(), AnkiError);
    }
    // Nothing listening there.
    AnkiConnectClient nobody(QStringLiteral("http://127.0.0.1:1"), {}, 500, 500);
    REQUIRE_THROWS_AS(nobody.Version(), AnkiError);
    AnkiConnectClient empty{QString()};
    REQUIRE_THROWS_AS(empty.Version(), AnkiError);
    AnkiConnectClient wrong(QStringLiteral("ftp://127.0.0.1"));
    REQUIRE_THROWS_AS(wrong.Version(), AnkiError);
}
