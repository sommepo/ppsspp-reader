// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The controller's buttons that no game uses, for the reader: the four grip buttons on the
// back of a Steam Controller (2026) and its "..." button. They are read here, beside the
// emulator and without its knowing: the emulator maps what a console's controller has, and
// these are not among it, so nothing has to be kept from the game.
//
// Where they come from:
//  - Linux: any controller the system offers whose buttons include the "extra" ones
//    (BTN_TRIGGER_HAPPY1 to 5). That is what the Steam Controller bridge on a Batocera box
//    turns the grips into (sc_bridge.py), and what the kernel calls an Elite pad's paddles.
//  - Windows: the Steam Controller's own reports, read from the device beside whatever else
//    reads it (Steam, or nobody). Nothing is written to the controller.
//
// The same file is in every reader fork.

#include <memory>
#include <QObject>

namespace ReaderUi {

class ReaderPad : public QObject {
    Q_OBJECT

public:
    /// How many buttons there are: the grips are 1 to 4, the "..." button is 5.
    static constexpr int Buttons = 5;

    explicit ReaderPad(QObject* parent = nullptr);
    ~ReaderPad() override;

    /// As if the button had been pressed (for the development channel and for tests).
    void Inject(int button);
    /// How many controllers with such buttons are being listened to.
    [[nodiscard]] int Sources() const;

signals:
    /// Button 1 to 5 went down.
    void Pressed(int button);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace ReaderUi
