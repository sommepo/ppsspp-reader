// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QLocale>
#include <QtEndian>
#include "reader/desktop/discord_presence.h"
#include "reader/desktop/stats.h"

namespace ReaderApp {

namespace {

/// Where the Discord program listens: a named pipe on Windows, a socket file elsewhere.
QStringList DefaultPipes() {
    QStringList names;
#ifdef _WIN32
    for (int i = 0; i < 10; ++i) {
        names.append(QStringLiteral("discord-ipc-%1").arg(i));
    }
#else
    QStringList folders;
    for (const char* variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const QString folder = qEnvironmentVariable(variable);
        if (!folder.isEmpty() && !folders.contains(folder)) {
            folders.append(folder);
        }
    }
    folders.append(QStringLiteral("/tmp"));
    for (const QString& folder : folders) {
        for (int i = 0; i < 10; ++i) {
            names.append(QDir(folder).filePath(QStringLiteral("discord-ipc-%1").arg(i)));
        }
    }
#endif
    return names;
}

/// Discord takes texts of 2 to 128 characters.
QString Fitted(QString text) {
    text = text.simplified();
    if (text.size() > 128) {
        text = text.left(127) + QChar(0x2026);
    }
    while (text.size() < 2) {
        text.append(QChar(0x3000));
    }
    return text;
}

} // namespace

DiscordPresence::DiscordPresence(QObject* parent) : QObject(parent), pipes{DefaultPipes()} {
    send_timer.setSingleShot(true);
    retry_timer.setSingleShot(true);
    connect(&send_timer, &QTimer::timeout, this, &DiscordPresence::SendNow);
    connect(&retry_timer, &QTimer::timeout, this, &DiscordPresence::Connect);
    connect(&socket, &QLocalSocket::connected, this, &DiscordPresence::OnConnected);
    connect(&socket, &QLocalSocket::readyRead, this, &DiscordPresence::OnReadyRead);
    connect(&socket, &QLocalSocket::disconnected, this, &DiscordPresence::OnGone);
    connect(&socket, &QLocalSocket::errorOccurred, this, [this] {
        // Not from inside the socket's own signal: it is not done with itself yet.
        if (!ready && !connecting_failed) {
            connecting_failed = true;
            QTimer::singleShot(0, this, [this] {
                connecting_failed = false;
                if (!ready && socket.state() == QLocalSocket::UnconnectedState) {
                    TryNextPipe();
                }
            });
        }
    });
}

DiscordPresence::~DiscordPresence() {
    socket.disconnect(this);
    socket.abort();
}

bool DiscordPresence::IsApplicationId(const QString& text) {
    return text.size() >= 17 && text.size() <= 20 &&
           std::all_of(text.begin(), text.end(),
                       [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); });
}

QString DiscordPresence::StateText(const Activity& activity) {
    const QLocale english{QLocale::English};
    QString text = english.toString(activity.characters) + QChar(0x5b57); // 字
    if (activity.active_ms >= SessionSummary::SpeedAfterMs && activity.characters > 0) {
        const qint64 per_hour = activity.characters * 3600000 / activity.active_ms;
        text += QStringLiteral(" · ") + english.toString(per_hour) + QChar(0x5b57) +
                QLatin1Char('/') + QChar(0x6642); // 字/時
    }
    if (activity.paused) {
        text += QStringLiteral(" · paused");
    }
    return text;
}

QJsonObject DiscordPresence::ActivityJson(const Activity& activity) {
    QJsonObject json;
    json.insert(QStringLiteral("type"), 0);
    json.insert(QStringLiteral("details"), Fitted(activity.game));
    json.insert(QStringLiteral("state"), Fitted(StateText(activity)));
    if (activity.started_s > 0) {
        json.insert(QStringLiteral("timestamps"),
                    QJsonObject{{QStringLiteral("start"), activity.started_s}});
    }
    return json;
}

QByteArray DiscordPresence::Frame(quint32 opcode, const QJsonObject& payload) {
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray frame(8, '\0');
    qToLittleEndian<quint32>(opcode, frame.data());
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), frame.data() + 4);
    return frame + body;
}

void DiscordPresence::SetPipes(const QStringList& names) {
    pipes = names;
}

void DiscordPresence::SetPace(int update_ms_, int retry_ms_) {
    update_ms = update_ms_;
    retry_ms = retry_ms_;
}

QString DiscordPresence::Status() const {
    if (!enabled) {
        return QStringLiteral("Off.");
    }
    if (!IsApplicationId(application_id)) {
        return QStringLiteral("No application ID: a number of 17 to 20 digits from Discord's "
                              "developer page.");
    }
    return ready ? QStringLiteral("Connected to Discord.")
                 : QStringLiteral("Discord is not running here (looked for now and then).");
}

void DiscordPresence::Configure(bool enabled_, const QString& application_id_) {
    const QString id = application_id_.trimmed();
    if (enabled == enabled_ && application_id == id) {
        return;
    }
    enabled = enabled_;
    application_id = id;
    // Another application, or none: the connection is of no use any more.
    retry_timer.stop();
    send_timer.stop();
    socket.abort();
    ready = false;
    told = false;
    sent.reset();
    incoming.clear();
    emit StatusChanged();
    if (enabled && IsApplicationId(application_id) && wanted) {
        Connect();
    }
}

void DiscordPresence::Set(const std::optional<Activity>& activity) {
    if (wanted == activity) {
        return;
    }
    wanted = activity;
    if (!enabled || !IsApplicationId(application_id)) {
        return;
    }
    if (ready) {
        Schedule();
    } else if (wanted && socket.state() == QLocalSocket::UnconnectedState &&
               !retry_timer.isActive()) {
        Connect();
    }
}

void DiscordPresence::Connect() {
    if (!enabled || !IsApplicationId(application_id) || !wanted ||
        socket.state() != QLocalSocket::UnconnectedState) {
        return;
    }
    next_pipe = 0;
    TryNextPipe();
}

void DiscordPresence::TryNextPipe() {
    if (!enabled || !wanted) {
        return;
    }
    if (next_pipe >= pipes.size()) {
        // Discord is not there. Looked for again later, while there is something to show.
        retry_timer.start(retry_ms);
        return;
    }
    socket.abort();
    socket.connectToServer(pipes.at(next_pipe++));
}

void DiscordPresence::OnConnected() {
    incoming.clear();
    socket.write(Frame(0, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("client_id"), application_id}}));
}

void DiscordPresence::OnReadyRead() {
    incoming += socket.readAll();
    while (incoming.size() >= 8) {
        const quint32 opcode = qFromLittleEndian<quint32>(incoming.constData());
        const quint32 length = qFromLittleEndian<quint32>(incoming.constData() + 4);
        if (length > 1 << 20) {
            socket.abort();
            return;
        }
        if (incoming.size() < 8 + static_cast<qsizetype>(length)) {
            return;
        }
        const QJsonObject message =
            QJsonDocument::fromJson(incoming.mid(8, static_cast<qsizetype>(length))).object();
        incoming.remove(0, 8 + static_cast<qsizetype>(length));
        if (opcode == 2) {
            // Discord closes: the application ID is not one it knows, most likely.
            socket.abort();
            return;
        }
        if (opcode == 3) {
            socket.write(Frame(4, message)); // a ping is answered with its own words
            continue;
        }
        if (!ready && message.value(QStringLiteral("evt")).toString() == QStringLiteral("READY")) {
            ready = true;
            told = false;
            sent.reset();
            last_send_ms = 0;
            emit StatusChanged();
            Schedule();
        }
    }
}

void DiscordPresence::OnGone() {
    const bool was_ready = ready;
    ready = false;
    told = false;
    sent.reset();
    send_timer.stop();
    if (was_ready) {
        emit StatusChanged();
    }
    if (enabled && wanted && !retry_timer.isActive()) {
        retry_timer.start(retry_ms);
    }
}

void DiscordPresence::Schedule() {
    if (!ready || (told && sent == wanted) || send_timer.isActive()) {
        return;
    }
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - last_send_ms;
    send_timer.start(static_cast<int>(std::clamp<qint64>(update_ms - since, 0, update_ms)));
}

void DiscordPresence::SendNow() {
    if (!ready || (told && sent == wanted)) {
        return;
    }
    QJsonObject arguments{{QStringLiteral("pid"), QCoreApplication::applicationPid()}};
    arguments.insert(QStringLiteral("activity"),
                     wanted ? QJsonValue(ActivityJson(*wanted)) : QJsonValue());
    socket.write(Frame(1, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
                                      {QStringLiteral("args"), arguments},
                                      {QStringLiteral("nonce"), QString::number(++nonce)}}));
    sent = wanted;
    told = true;
    last_send_ms = QDateTime::currentMSecsSinceEpoch();
}

} // namespace ReaderApp
