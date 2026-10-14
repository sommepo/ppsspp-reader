// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include "reader/desktop/text_line.h"

class QTcpServer;
class QTcpSocket;

namespace ReaderApp {

/**
 * A small WebSocket server that sends every newly presented line to connected clients, the way
 * texthooker pages and sentence-mining tools expect. Nothing is replayed to a client that
 * connects later, and nothing a client sends is acted on.
 *
 * It listens on the loopback interface unless `lan` is set. There is no authentication:
 * anything that can reach the port can read the text, which is why other devices are opt-in.
 * Lives on the thread it was made on (the app's main thread).
 */
class TextHookServer : public QObject {
    Q_OBJECT

public:
    TextHookServer(int port_, bool lan_, QObject* parent = nullptr);
    ~TextHookServer() override;

    /// "plain", "sentence" or "json".
    QString format{QStringLiteral("plain")};
    bool include_speaker{};
    QString game_id;

    /// Starts listening. False when the port cannot be bound.
    bool Start();
    void Stop();

    [[nodiscard]] int Port() const {
        return port;
    }
    [[nodiscard]] bool Lan() const {
        return lan;
    }
    /// The port actually bound; differs from Port() only when that was 0.
    [[nodiscard]] int BoundPort() const;
    [[nodiscard]] int ClientCount() const {
        return static_cast<int>(clients.size());
    }
    /// A ws:// address for the settings screen.
    [[nodiscard]] QString DescribeAddress() const;

    /// Sends `line` to every connected client.
    void Broadcast(const TextLine& line);
    /// The message sent for `line` in the current format.
    [[nodiscard]] QString Encode(const TextLine& line) const;

    /// The Sec-WebSocket-Accept value for a client's key (RFC 6455, section 4.2.2).
    [[nodiscard]] static QByteArray AcceptKey(const QByteArray& key);
    /// A final, unmasked server frame.
    [[nodiscard]] static QByteArray Frame(int opcode, const QByteArray& payload);
    [[nodiscard]] static QByteArray TextFrame(const QByteArray& payload);

private:
    void OnConnection();
    void OnData(QTcpSocket* socket);
    void Drop(QTcpSocket* socket);

    int port;
    bool lan;
    QTcpServer* server{};
    /// What has arrived from a connection and is not dealt with yet.
    QHash<QTcpSocket*, QByteArray> buffers;
    /// Connections that have completed the WebSocket handshake.
    QList<QTcpSocket*> clients;
};

} // namespace ReaderApp
