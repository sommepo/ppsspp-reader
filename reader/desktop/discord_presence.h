// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QByteArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace ReaderApp {

/**
 * Shows in the user's Discord status what is being read: the game, the characters read in
 * this session and the reading speed, with the time since the session began.
 *
 * It talks to the Discord program on the same computer through Discord's own local pipe
 * ("discord-ipc-0" to "-9"): a handshake that names a Discord application (whose name is what
 * the status says is being played; the user registers one and enters its ID), then
 * SET_ACTIVITY whenever the numbers changed, at most once in fifteen seconds, which is what
 * Discord takes. Nothing leaves the computer from here. Without Discord running nothing
 * happens; it is looked for again now and then.
 */
class DiscordPresence : public QObject {
    Q_OBJECT

public:
    struct Activity {
        QString game;
        qint64 characters{};
        qint64 active_ms{};
        /// When the session began, in seconds since 1970.
        qint64 started_s{};
        bool paused{};

        bool operator==(const Activity&) const = default;
    };

    explicit DiscordPresence(QObject* parent = nullptr);
    ~DiscordPresence() override;

    /// Switches it on or off and names the Discord application. Off, the status is taken back.
    void Configure(bool enabled, const QString& application_id);
    /// What is to be shown from now on; nothing takes the status back.
    void Set(const std::optional<Activity>& activity);

    /// Connected to Discord and greeted by it.
    [[nodiscard]] bool IsReady() const {
        return ready;
    }
    /// A sentence about how it stands, for the settings.
    [[nodiscard]] QString Status() const;

    /// "1,234字 · 9,800字/時": the speed only once enough was read for it to mean something.
    [[nodiscard]] static QString StateText(const Activity& activity);
    /// The activity as Discord takes it.
    [[nodiscard]] static QJsonObject ActivityJson(const Activity& activity);
    /// A message as it travels: the kind (0 handshake, 1 command), the length, the JSON.
    [[nodiscard]] static QByteArray Frame(quint32 opcode, const QJsonObject& payload);
    /// An application ID is a number of 17 to 20 digits.
    [[nodiscard]] static bool IsApplicationId(const QString& text);

    /// For tests: where Discord is looked for, and how long between two updates and two looks.
    void SetPipes(const QStringList& names);
    void SetPace(int update_ms, int retry_ms);

signals:
    void StatusChanged();

private:
    void Connect();
    void TryNextPipe();
    void OnConnected();
    void OnReadyRead();
    void OnGone();
    void SendNow();
    void Schedule();

    QLocalSocket socket;
    QTimer send_timer;
    QTimer retry_timer;
    QStringList pipes;
    int next_pipe{};
    bool enabled{};
    bool ready{};
    bool connecting_failed{};
    QString application_id;
    std::optional<Activity> wanted;
    /// What Discord was last told; `told` is false until something was sent in this connection.
    std::optional<Activity> sent;
    bool told{};
    qint64 last_send_ms{};
    int update_ms{15000};
    int retry_ms{30000};
    quint64 nonce{};
    QByteArray incoming;
};

} // namespace ReaderApp
