// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include "reader/desktop/text_hook_server.h"

namespace ReaderApp {

namespace {

constexpr qsizetype MaxHeaderBytes = 16 * 1024;
constexpr quint64 MaxClientFrame = 64 * 1024;
constexpr int OpcodeText = 0x1;
constexpr int OpcodeClose = 0x8;
constexpr int OpcodePing = 0x9;
constexpr int OpcodePong = 0xA;
constexpr const char* WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

} // namespace

TextHookServer::TextHookServer(int port_, bool lan_, QObject* parent)
    : QObject(parent), port{port_}, lan{lan_} {}

TextHookServer::~TextHookServer() {
    Stop();
}

bool TextHookServer::Start() {
    if (server) {
        return true;
    }
    server = new QTcpServer(this);
    const QHostAddress address = lan ? QHostAddress(QHostAddress::AnyIPv4)
                                     : QHostAddress(QHostAddress::LocalHost);
    if (!server->listen(address, static_cast<quint16>(port))) {
        delete server;
        server = nullptr;
        return false;
    }
    connect(server, &QTcpServer::newConnection, this, &TextHookServer::OnConnection);
    return true;
}

void TextHookServer::Stop() {
    const QList<QTcpSocket*> open = buffers.keys();
    buffers.clear();
    clients.clear();
    for (QTcpSocket* socket : open) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (server) {
        server->close();
        delete server;
        server = nullptr;
    }
}

int TextHookServer::BoundPort() const {
    return server ? server->serverPort() : -1;
}

QString TextHookServer::DescribeAddress() const {
    QString host = QStringLiteral("127.0.0.1");
    if (lan) {
        for (const QHostAddress& address : QNetworkInterface::allAddresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
                host = address.toString();
                break;
            }
        }
    }
    return QStringLiteral("ws://%1:%2").arg(host).arg(BoundPort());
}

QString TextHookServer::Encode(const TextLine& line) const {
    const bool with_speaker = include_speaker && !line.speaker.isEmpty();
    const QString sentence = with_speaker ? line.speaker + u'\n' + line.text : line.text;
    if (format == QStringLiteral("sentence")) {
        return QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("sentence"), sentence}})
                .toJson(QJsonDocument::Compact));
    }
    if (format == QStringLiteral("json")) {
        QJsonObject event;
        // The 3DS sibling's format, which tools already read.
        event.insert(QStringLiteral("format"), QStringLiteral("azahar-reader.text-event"));
        event.insert(QStringLiteral("version"), 1);
        event.insert(QStringLiteral("gameId"), game_id);
        event.insert(QStringLiteral("segmentId"), line.source_id);
        event.insert(QStringLiteral("channel"), line.channel);
        event.insert(QStringLiteral("speaker"), line.speaker);
        event.insert(QStringLiteral("sentence"), sentence);
        event.insert(QStringLiteral("ruby"), TextLine::RubyToJson(line.ruby));
        event.insert(QStringLiteral("timestamp"),
                     QDateTime::fromMSecsSinceEpoch(line.timestamp, QTimeZone::UTC)
                         .toString(Qt::ISODateWithMs));
        event.insert(QStringLiteral("speakerIncluded"), with_speaker);
        return QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
    }
    return sentence;
}

void TextHookServer::Broadcast(const TextLine& line) {
    if (!server || clients.isEmpty()) {
        return;
    }
    const QByteArray frame = TextFrame(Encode(line).toUtf8());
    for (QTcpSocket* client : std::as_const(clients)) {
        client->write(frame);
    }
}

QByteArray TextHookServer::AcceptKey(const QByteArray& key) {
    return QCryptographicHash::hash(key + WebSocketGuid, QCryptographicHash::Sha1).toBase64();
}

QByteArray TextHookServer::TextFrame(const QByteArray& payload) {
    return Frame(OpcodeText, payload);
}

QByteArray TextHookServer::Frame(int opcode, const QByteArray& payload) {
    QByteArray out;
    out.append(static_cast<char>(0x80 | opcode));
    const quint64 size = static_cast<quint64>(payload.size());
    if (size < 126) {
        out.append(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        out.append(static_cast<char>(126));
        out.append(static_cast<char>(size >> 8));
        out.append(static_cast<char>(size & 0xFF));
    } else {
        out.append(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.append(static_cast<char>((size >> (8 * i)) & 0xFF));
        }
    }
    return out + payload;
}

void TextHookServer::OnConnection() {
    while (QTcpSocket* socket = server->nextPendingConnection()) {
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        buffers.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { OnData(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { Drop(socket); });
    }
}

void TextHookServer::Drop(QTcpSocket* socket) {
    if (!buffers.contains(socket)) {
        return;
    }
    buffers.remove(socket);
    clients.removeAll(socket);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void TextHookServer::OnData(QTcpSocket* socket) {
    auto found = buffers.find(socket);
    if (found == buffers.end()) {
        return;
    }
    QByteArray& buffer = *found;
    buffer += socket->readAll();

    if (!clients.contains(socket)) {
        // The handshake: a GET with an Upgrade header and a key.
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (buffer.size() > MaxHeaderBytes) {
                Drop(socket);
            }
            return;
        }
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        buffer.remove(0, end + 4);
        QHash<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
            }
        }
        const QByteArray key = headers.value("sec-websocket-key");
        if (!lines.value(0).startsWith("GET ") || key.isEmpty() ||
            headers.value("upgrade").toLower() != "websocket") {
            const QByteArray body = "PPSSPP Reader text server. Connect with a WebSocket client.\n";
            socket->write("HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain; "
                          "charset=utf-8\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->flush();
            Drop(socket);
            return;
        }
        socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                      AcceptKey(key) + "\r\n\r\n");
        clients.append(socket);
    }

    // Client frames: pings are answered, a close frame ends the connection, the rest is
    // read and dropped.
    while (true) {
        if (buffer.size() < 2) {
            return;
        }
        const quint8 b0 = static_cast<quint8>(buffer.at(0));
        const quint8 b1 = static_cast<quint8>(buffer.at(1));
        const int opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        quint64 length = b1 & 0x7F;
        qsizetype header = 2;
        if (length == 126) {
            if (buffer.size() < 4) {
                return;
            }
            length = (static_cast<quint64>(static_cast<quint8>(buffer.at(2))) << 8) |
                     static_cast<quint8>(buffer.at(3));
            header = 4;
        } else if (length == 127) {
            if (buffer.size() < 10) {
                return;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<quint8>(buffer.at(2 + i));
            }
            header = 10;
        }
        if (length > MaxClientFrame) {
            Drop(socket);
            return;
        }
        const qsizetype mask_at = header;
        if (masked) {
            header += 4;
        }
        if (buffer.size() < header + static_cast<qsizetype>(length)) {
            return;
        }
        QByteArray payload = buffer.mid(header, static_cast<qsizetype>(length));
        if (masked) {
            for (qsizetype i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload.at(i) ^ buffer.at(mask_at + (i & 3)));
            }
        }
        buffer.remove(0, header + static_cast<qsizetype>(length));
        if (opcode == OpcodeClose) {
            socket->write(Frame(OpcodeClose, {}));
            socket->flush();
            Drop(socket);
            return;
        }
        if (opcode == OpcodePing) {
            socket->write(Frame(OpcodePong, payload));
        }
    }
}

} // namespace ReaderApp
