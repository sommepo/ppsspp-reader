// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <utility>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUrl>
#include <QRegularExpression>
#include "reader/desktop/anki.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp {

namespace {

constexpr qsizetype MaxHeaderBytes = 16 * 1024;
constexpr qsizetype MaxReplyBytes = 8 * 1024 * 1024;

QStringList Strings(const QJsonValue& value) {
    if (!value.isArray()) {
        throw AnkiError(QStringLiteral("Unexpected reply from AnkiConnect"));
    }
    QStringList out;
    for (const QJsonValue& item : value.toArray()) {
        out.append(item.toString());
    }
    return out;
}

/// Reads until `socket` has at least `count` bytes, it closes, or time runs out.
bool WaitForBytes(QTcpSocket& socket, qsizetype count, QElapsedTimer& timer, int timeout_ms) {
    while (socket.bytesAvailable() < count) {
        const qint64 left = timeout_ms - timer.elapsed();
        if (left <= 0) {
            return false;
        }
        if (!socket.waitForReadyRead(static_cast<int>(left))) {
            return socket.bytesAvailable() >= count;
        }
    }
    return true;
}

} // namespace

QString AnkiSearch::Escape(const QString& text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar c : text) {
        if (c == u'\\' || c == u'"' || c == u'*' || c == u'_') {
            out += u'\\';
        }
        out += c;
    }
    return out;
}

QString AnkiSearch::FieldIs(const QString& deck, const QString& field, const QString& value) {
    QString out;
    if (!deck.isEmpty()) {
        out += QStringLiteral("\"deck:") + Escape(deck) + QStringLiteral("\" ");
    }
    // A colon in the field's name would end the name early; one in the value is harmless.
    QString name = Escape(field);
    name.replace(u':', QStringLiteral("\\:"));
    out += u'"' + name + u':' + Escape(value) + u'"';
    return out;
}

QString AnkiSearch::Notes(const QList<qint64>& note_ids) {
    QStringList ids;
    for (const qint64 id : note_ids) {
        ids.append(QString::number(id));
    }
    return QStringLiteral("nid:") + ids.join(u',');
}

namespace SourceAddress {

namespace {
const QRegularExpression& Address() {
    static const QRegularExpression address{QStringLiteral("https?://[^\\s<>\"']+")};
    return address;
}
} // namespace

QString Of(const QString& title_id) {
    static const QRegularExpression id{QStringLiteral("^[a-z]{4}[0-9]{5}$")};
    const QString lower = title_id.trimmed().toLower();
    return id.match(lower).hasMatch() ? QStringLiteral("https://www.jp.playstation.com/software/title/%1.html").arg(lower)
                                      : QString();
}

QString AskedFor(const QString& error) {
    static const QRegularExpression shapes[] = {
        QRegularExpression{QStringLiteral("^Map (.+?) to \\{url\\} in Yomitan")},
        QRegularExpression{QStringLiteral("^The (.+?) field must contain Yomitan.s \\{url\\}")},
    };
    const QString text = error.trimmed();
    for (const QRegularExpression& shape : shapes) {
        const QRegularExpressionMatch match = shape.match(text);
        if (match.hasMatch() && !match.captured(1).trimmed().isEmpty()) {
            return match.captured(1);
        }
    }
    return {};
}

bool HasAddress(const QString& value) {
    return Address().match(value).hasMatch();
}

QMap<QString, QString> With(const QMap<QString, QString>& fields, const QString& field,
                            const QString& url, const QString& title) {
    const QString held = fields.value(field);
    if (url.isEmpty() || field.isEmpty() || HasAddress(held)) {
        return fields;
    }
    QString shown = held.trimmed().isEmpty() ? EscapeHtml(title) : held;
    if (shown.trimmed().isEmpty()) {
        shown = EscapeHtml(url);
    }
    QMap<QString, QString> out = fields;
    out.insert(field, QStringLiteral("<a href=\"%1\">%2</a>").arg(EscapeHtml(url), shown));
    return out;
}

Added AddNote(const std::function<qint64(const QMap<QString, QString>&)>& add,
              const QMap<QString, QString>& fields, const QString& url, const QString& title,
              const QString& known) {
    const QMap<QString, QString> first = known.isEmpty() ? fields : With(fields, known, url, title);
    try {
        return {add(first), first == fields ? QString() : known};
    } catch (const AnkiError& error) {
        const QString field = AskedFor(error.Message());
        if (field.isEmpty()) {
            throw;
        }
        const QMap<QString, QString> second = With(first, field, url, title);
        if (second == first) {
            throw; // nothing to give, or it has one and is refused all the same
        }
        return {add(second), field};
    }
}

} // namespace SourceAddress

AnkiConnectClient::AnkiConnectClient(QString url_, QString key_, int connect_timeout_ms_,
                                     int read_timeout_ms_)
    : url{std::move(url_)}, key{std::move(key_)}, connect_timeout_ms{connect_timeout_ms_},
      read_timeout_ms{read_timeout_ms_} {}

QStringList AnkiConnectClient::DeckNames() {
    return Strings(Invoke(QStringLiteral("deckNames")));
}

QStringList AnkiConnectClient::ModelNames() {
    return Strings(Invoke(QStringLiteral("modelNames")));
}

QStringList AnkiConnectClient::FieldNames(const QString& model) {
    const QJsonObject params{{QStringLiteral("modelName"), model}};
    return Strings(Invoke(QStringLiteral("modelFieldNames"), &params));
}

QString AnkiConnectClient::StoreMedia(const QByteArray& data, const QString& preferred_name) {
    const QJsonObject params{{QStringLiteral("filename"), preferred_name},
                             {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}};
    const QJsonValue result = Invoke(QStringLiteral("storeMediaFile"), &params);
    const QString stored = result.toString();
    return stored.isEmpty() ? preferred_name : stored;
}

qint64 AnkiConnectClient::AddNote(const QString& deck, const QString& model,
                                  const QMap<QString, QString>& fields, const QStringList& tags,
                                  Duplicates duplicates) {
    QJsonObject options{{QStringLiteral("allowDuplicate"), duplicates == Duplicates::Allow}};
    if (duplicates == Duplicates::RefuseInDeck) {
        options.insert(QStringLiteral("duplicateScope"), QStringLiteral("deck"));
        options.insert(QStringLiteral("duplicateScopeOptions"),
                       QJsonObject{{QStringLiteral("deckName"), deck},
                                   {QStringLiteral("checkChildren"), true}});
    }
    QJsonObject field_values;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        field_values.insert(it.key(), it.value());
    }
    const QJsonObject note{{QStringLiteral("deckName"), deck},
                           {QStringLiteral("modelName"), model},
                           {QStringLiteral("fields"), field_values},
                           {QStringLiteral("tags"), QJsonArray::fromStringList(tags)},
                           {QStringLiteral("options"), options}};
    const QJsonObject params{{QStringLiteral("note"), note}};
    const QJsonValue result = Invoke(QStringLiteral("addNote"), &params);
    if (!result.isDouble()) {
        throw AnkiError(QStringLiteral("Anki did not create the note"));
    }
    return result.toInteger();
}

QList<qint64> AnkiConnectClient::FindNotes(const QString& deck, const QString& field,
                                           const QString& value) {
    const QJsonObject params{{QStringLiteral("query"), AnkiSearch::FieldIs(deck, field, value)}};
    const QJsonValue result = Invoke(QStringLiteral("findNotes"), &params);
    if (!result.isArray()) {
        throw AnkiError(QStringLiteral("Unexpected reply from AnkiConnect"));
    }
    QList<qint64> out;
    for (const QJsonValue& id : result.toArray()) {
        out.append(id.toInteger());
    }
    return out;
}

void AnkiConnectClient::ShowNotes(const QList<qint64>& note_ids) {
    if (note_ids.isEmpty()) {
        return;
    }
    const QJsonObject params{{QStringLiteral("query"), AnkiSearch::Notes(note_ids)}};
    Invoke(QStringLiteral("guiBrowse"), &params);
}

int AnkiConnectClient::Version() {
    const QJsonValue result = Invoke(QStringLiteral("version"));
    if (!result.isDouble()) {
        throw AnkiError(QStringLiteral("Unexpected reply from AnkiConnect"));
    }
    return result.toInt();
}

QByteArray AnkiConnectClient::RequestBody(const QString& action, const QJsonObject* params) const {
    QJsonObject request{{QStringLiteral("action"), action}, {QStringLiteral("version"), 6}};
    if (params) {
        request.insert(QStringLiteral("params"), *params);
    }
    if (!key.isEmpty()) {
        request.insert(QStringLiteral("key"), key);
    }
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

QJsonValue AnkiConnectClient::Invoke(const QString& action, const QJsonObject* params) {
    const QByteArray text = Post(RequestBody(action, params));
    const QJsonDocument reply = QJsonDocument::fromJson(text);
    if (!reply.isObject()) {
        throw AnkiError(QStringLiteral("AnkiConnect sent something that is not JSON"));
    }
    const QJsonObject object = reply.object();
    const QJsonValue error = object.value(QStringLiteral("error"));
    if (!error.isNull() && !error.isUndefined()) {
        throw AnkiError(error.isString() ? error.toString()
                                         : QStringLiteral("AnkiConnect reported an error"));
    }
    return object.value(QStringLiteral("result"));
}

/// One HTTP/1.1 POST over a socket of the app's own; the reply's body.
QByteArray AnkiConnectClient::Post(const QByteArray& body) {
    // "192.168.1.5:8765" is what people type; that is http.
    const QString address = url.trimmed();
    if (address.isEmpty()) {
        throw AnkiError(QStringLiteral(
            "Enter the address of AnkiConnect, for example http://127.0.0.1:8765"));
    }
    const QUrl target(address.contains(QStringLiteral("://")) ? address
                                                             : QStringLiteral("http://") + address);
    const QString scheme = target.scheme().toLower();
    const bool secure = scheme == QStringLiteral("https");
    if (!secure && scheme != QStringLiteral("http")) {
        throw AnkiError(
            QStringLiteral("The AnkiConnect address must start with http:// or https://"));
    }
    const QString host = target.host();
    if (host.isEmpty()) {
        throw AnkiError(QStringLiteral("The AnkiConnect address has no host name"));
    }
    const int port = target.port(secure ? 443 : 80);
    const auto unreachable = [this](const QString& why) {
        return AnkiError(QStringLiteral("Cannot reach AnkiConnect at %1: %2").arg(url, why));
    };

    std::unique_ptr<QTcpSocket> socket;
    if (secure) {
        auto encrypted = std::make_unique<QSslSocket>();
        encrypted->connectToHostEncrypted(host, static_cast<quint16>(port));
        if (!encrypted->waitForEncrypted(connect_timeout_ms)) {
            throw unreachable(encrypted->errorString());
        }
        socket = std::move(encrypted);
    } else {
        socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(host, static_cast<quint16>(port));
        if (!socket->waitForConnected(connect_timeout_ms)) {
            throw unreachable(socket->errorString());
        }
    }

    QString path = target.path(QUrl::FullyEncoded);
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    if (target.hasQuery()) {
        path += u'?' + target.query(QUrl::FullyEncoded);
    }
    const QString host_header = host.contains(u':') ? u'[' + host + u']' : host;
    QByteArray request = "POST " + path.toLatin1() + " HTTP/1.1\r\nHost: " +
                         host_header.toLatin1() + ":" + QByteArray::number(port) +
                         "\r\nContent-Type: application/json\r\nContent-Length: " +
                         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
    request += body;
    socket->write(request);
    while (socket->bytesToWrite() > 0) {
        if (!socket->waitForBytesWritten(read_timeout_ms)) {
            throw unreachable(socket->errorString());
        }
    }

    QElapsedTimer timer;
    timer.start();
    // Headers.
    QByteArray head;
    while (true) {
        const qsizetype end = head.indexOf("\r\n\r\n");
        if (end >= 0) {
            break;
        }
        if (head.size() > MaxHeaderBytes) {
            throw AnkiError(QStringLiteral("AnkiConnect sent an overlong header"));
        }
        if (!WaitForBytes(*socket, 1, timer, read_timeout_ms)) {
            if (head.isEmpty()) {
                throw unreachable(socket->state() == QAbstractSocket::UnconnectedState
                                      ? QStringLiteral("the connection was closed")
                                      : QStringLiteral("no answer in time"));
            }
            throw AnkiError(QStringLiteral("AnkiConnect sent something that is not HTTP"));
        }
        // One byte at a time near the end keeps the body in the socket.
        head += socket->read(1);
    }
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> status_line = lines.value(0).trimmed().split(' ');
    bool status_ok = false;
    const int status = status_line.value(1).toInt(&status_ok);
    if (!status_ok) {
        throw AnkiError(QStringLiteral("AnkiConnect sent something that is not HTTP"));
    }
    qsizetype length = -1;
    bool chunked = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            bool ok = false;
            const qsizetype parsed = value.toLongLong(&ok);
            length = ok ? parsed : -1;
        } else if (name == "transfer-encoding") {
            chunked = value.toLower().contains("chunked");
        }
    }
    if (status != 200) {
        throw AnkiError(QStringLiteral("AnkiConnect answered HTTP %1").arg(status));
    }

    const auto read_exactly = [&](qsizetype count) {
        if (!WaitForBytes(*socket, count, timer, read_timeout_ms)) {
            // What there is: a short reply fails as JSON further on.
            return socket->readAll();
        }
        return socket->read(count);
    };
    const auto read_line = [&]() {
        QByteArray line;
        while (!line.endsWith('\n')) {
            if (!WaitForBytes(*socket, 1, timer, read_timeout_ms)) {
                break;
            }
            line += socket->read(1);
            if (line.size() > MaxHeaderBytes) {
                throw AnkiError(QStringLiteral("AnkiConnect sent an overlong header"));
            }
        }
        return line.trimmed();
    };

    QByteArray reply;
    if (chunked) {
        while (true) {
            const QByteArray size_line = read_line();
            bool ok = false;
            const qsizetype size = size_line.split(';').value(0).trimmed().toLongLong(&ok, 16);
            if (!ok || size <= 0) {
                break;
            }
            if (reply.size() + size > MaxReplyBytes) {
                throw AnkiError(QStringLiteral("AnkiConnect's reply is too large"));
            }
            reply += read_exactly(size);
            read_line(); // the line break after the chunk
        }
    } else if (length >= 0) {
        reply = read_exactly(std::min(length, MaxReplyBytes));
    } else {
        while (socket->state() == QAbstractSocket::ConnectedState &&
               timer.elapsed() < read_timeout_ms && reply.size() <= MaxReplyBytes) {
            if (!socket->waitForReadyRead(200)) {
                continue;
            }
            reply += socket->readAll();
        }
        reply += socket->readAll();
    }
    socket->disconnectFromHost();
    return reply;
}

} // namespace ReaderApp
