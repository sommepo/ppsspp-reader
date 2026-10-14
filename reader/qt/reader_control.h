// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
class QMainWindow;
class QWidget;

namespace ReaderUi {

class ReaderController;

/**
 * A development tool, off unless the environment variable READER_DESKTOP_CONTROL names a
 * socket: lets a script drive the desktop app on a machine without a display (clicks in the
 * game's picture, key presses, pictures of what the reader's windows show) and ask what the
 * reader holds. This is how the desktop reader layer is tried against a real game on the
 * development machine (tools/reader/desktop.sh, tools/reader/dc.py); the Android app has the
 * device for that.
 *
 * One command per line; a connection gets "OK ..." or "ERR ..." for each, in order.
 */
class ReaderControl : public QObject {
    Q_OBJECT

public:
    /// Starts listening when the environment asks for it; does nothing otherwise.
    static void StartIfWanted(QMainWindow* main_window, QWidget* render_window,
                              ReaderController* controller);

private:
    ReaderControl(const QString& path, QMainWindow* main_window_, QWidget* render_window_,
                  ReaderController* controller_);

    struct Connection {
        QByteArray buffer;
        QStringList commands;
        bool finished_reading{};
        bool busy{};
    };

    using Reply = std::function<void(const QString&)>;

    void OnData(QLocalSocket* socket);
    void Pump(QLocalSocket* socket);
    void Run(const QString& command, const Reply& reply);
    [[nodiscard]] QString Status() const;
    [[nodiscard]] QString DescribeDialog() const;
    void Grab(const QString& file, const Reply& reply);
    void SendPointer(const QPointF& position, int kind, Qt::KeyboardModifiers modifiers);

    QLocalServer* server{};
    QMainWindow* main_window;
    QWidget* render_window;
    ReaderController* controller;
    QHash<QLocalSocket*, Connection> connections;
};

} // namespace ReaderUi
