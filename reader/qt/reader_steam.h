// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Valve's Steam Controller (2026) on Windows without Steam. The controller is no gamepad to
// Windows by itself: left alone it plays keyboard and mouse. While Steam is not running, the
// reader reads the controller's own reports (reader/desktop/steam_bridge has what they
// mean), switches that emulation off, and offers in its place
//  - a gamepad the emulator finds like any other: one made inside the program through the
//    emulator's own input library (SDL's "virtual joystick"), so no driver is installed;
//  - the right pad as the mouse pointer (a trackball) with a click when pressed, the left
//    pad as the wheel with the right mouse button when pressed.
// The grips and the "..." button stay the reader's own (reader_pad).
//
// On other systems nothing is read: there the system or a bridge of its own does this (the
// maintainer's Batocera box runs sc_bridge.py). The development channel can hand it a
// report anywhere, which is how the gamepad half is tried without a controller.
//
// The same file is in every reader fork.

#include <memory>
#include <QByteArray>
#include <QObject>
#include <QString>

namespace ReaderUi {

class SteamControllerBridge : public QObject {
    Q_OBJECT

public:
    explicit SteamControllerBridge(QObject* parent = nullptr);
    ~SteamControllerBridge() override;

    /// As if a controller had sent this report (for the development channel).
    bool InjectReport(const QByteArray& report);
    /// What it is doing, in a sentence (for the development channel and the log).
    [[nodiscard]] QString Describe() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace ReaderUi
