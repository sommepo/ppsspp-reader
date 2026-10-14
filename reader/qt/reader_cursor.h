// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QTimer>

namespace ReaderUi {

/**
 * The cursor of the game's picture, for playing with a controller: while its button is held
 * the sticks steer a cursor over the picture instead of reaching the game, and a word the
 * cursor rests on is looked up, so that a word can be reached without the mouse. As a
 * setting the button switches the cursor on and off instead. A press of a stick clicks where
 * the cursor is (the Android app has the second display's trackpad for that).
 *
 * This object holds the state: whether the cursor is in use, where it is, what the sticks say.
 * The emulated controllers ask about the cursor's button and pass the sticks on whatever
 * thread input arrives; everything else happens on the main thread.
 */
class StickCursor : public QObject {
    Q_OBJECT

public:
    explicit StickCursor(QObject* parent);
    ~StickCursor() override;

    /// Whether there is anything to point at: a game is running and its text is being read.
    void SetArmed(bool armed_);
    /// The button (a NativeButton number, -1 for none), the speed in percent, and whether the
    /// button switches the cursor on and off rather than having to be held.
    void Configure(int button, int speed_percent_, bool toggles_);
    /// The area the cursor stays in: the render window's.
    void SetBounds(const QRectF& bounds_);
    /// Tells the emulated controllers of the game to show the sticks as they are now, or at
    /// rest. Given by the controller, which knows the emulated system.
    std::function<void()> refresh_sticks;

    [[nodiscard]] bool IsActive() const {
        return active.load();
    }
    [[nodiscard]] QPointF Position() const {
        return position;
    }
    /// Puts the cursor somewhere without the player having moved it.
    void Place(const QPointF& point);
    /// Lets go of the cursor whoever holds it: the game was paused, or closed.
    void Cancel();
    /// Takes the handlers away from the emulated controllers. Before the game is closed.
    void Detach();
    /// Gives them to the emulated controllers. When a game was loaded.
    void Attach();

signals:
    /// The cursor was switched on or off.
    void ActiveChanged(bool active);
    /// The cursor is somewhere else. `by_player` is false when the reader put it there.
    void Moved(bool by_player);
    /// A stick was pressed: a click where the cursor is.
    void Clicked();

private:
    void OnButton(bool pressed);
    void OnStick(int stick, float x, float y);
    void SetActive(bool on);
    void Step();
    [[nodiscard]] qreal Deflection() const;

    std::atomic<bool> armed{false};
    std::atomic<bool> active{false};
    std::atomic<bool> toggles{false};
    int button{-1};
    int speed_percent{100};
    bool held{};
    QRectF bounds;
    QPointF position;
    QHash<int, QPointF> sticks;
    QTimer step_timer;
    QElapsedTimer clock;
    qint64 last_step{};
    bool attached{};
};

} // namespace ReaderUi
