// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include "reader/core/types.h"
#include "reader/core/input_filter.h"
#include "reader/qt/reader_cursor.h"

namespace ReaderUi {

namespace {

constexpr qreal DeadZone = 0.12;
constexpr qreal Curve = 1.7;
/// Seconds across the picture with a stick pushed all the way.
constexpr qreal CrossingSeconds = 1.0;
constexpr int FrameMs = 16;
constexpr qint64 MaxStepMs = 50;
/// The emulator's numbers for a press of the left and of the right stick (NativeButton).
constexpr int ButtonLStick = 106; // the emulator's key codes for a controller's stick presses
constexpr int ButtonRStick = 107;

} // namespace

StickCursor::StickCursor(QObject* parent) : QObject(parent) {
    step_timer.setInterval(FrameMs);
    connect(&step_timer, &QTimer::timeout, this, &StickCursor::Step);
    clock.start();
}

StickCursor::~StickCursor() {
    Detach();
}

void StickCursor::SetArmed(bool armed_) {
    armed = armed_;
    if (!armed_) {
        Cancel();
    }
}

void StickCursor::Configure(int button_, int speed_percent_, bool toggles_) {
    if ((button_ != button || toggles_ != toggles.load()) && active) {
        Cancel();
    }
    button = button_;
    speed_percent = std::clamp(speed_percent_, 10, 500);
    toggles = toggles_;
    if (attached) {
        Attach();
    }
}

void StickCursor::SetBounds(const QRectF& bounds_) {
    bounds = bounds_;
    if (bounds.isValid()) {
        position.setX(std::clamp(position.x(), bounds.left(), bounds.right() - 1));
        position.setY(std::clamp(position.y(), bounds.top(), bounds.bottom() - 1));
    }
}

void StickCursor::Attach() {
    attached = true;
    auto& filter = Reader::InputFilter::Instance();
    filter.SetButton(button);
    // Input arrives on whatever thread reads the controller. Whether the button is the
    // reader's has to be said at once, from what is known now; the rest follows on the main
    // thread.
    filter.SetHandlers(
        [this](bool pressed) {
            QMetaObject::invokeMethod(this, [this, pressed] { OnButton(pressed); },
                                      Qt::QueuedConnection);
            // A press is the reader's when there is something to point at. Its release is
            // then the reader's as well; the release of a press the game got goes to the
            // game whatever is said here.
            return pressed ? armed.load() : true;
        },
        [this](std::size_t stick, float x, float y) {
            QMetaObject::invokeMethod(
                this, [this, stick, x, y] { OnStick(static_cast<int>(stick), x, y); },
                Qt::QueuedConnection);
        });
    // The stick that steers also clicks, unless a stick's press is the cursor's own button.
    std::vector<int> clicks;
    for (const int click : {ButtonLStick, ButtonRStick}) {
        if (click != button) {
            clicks.push_back(click);
        }
    }
    filter.SetClickButtons(std::move(clicks), [this](bool pressed) {
        if (pressed) {
            QMetaObject::invokeMethod(this, [this] { emit Clicked(); }, Qt::QueuedConnection);
        }
    });
}

void StickCursor::Detach() {
    if (!attached) {
        return;
    }
    attached = false;
    Cancel();
    auto& filter = Reader::InputFilter::Instance();
    filter.SetHandlers(nullptr, nullptr);
    filter.SetClickButtons({}, nullptr);
    filter.SetButton(-1);
}

void StickCursor::OnButton(bool pressed) {
    if (button < 0) {
        return;
    }
    if (!armed) {
        Cancel();
        return;
    }
    if (toggles) {
        // A press counts once: a held key repeats.
        if (pressed && !held) {
            SetActive(!active);
        }
        held = pressed;
    } else {
        held = pressed;
        SetActive(pressed);
    }
}

void StickCursor::SetActive(bool on) {
    if (active == on) {
        return;
    }
    active = on;
    sticks.clear();
    step_timer.stop();
    // While the cursor is in use the game sees the sticks at rest.
    Reader::InputFilter::Instance().HoldSticks(on);
    if (refresh_sticks) {
        refresh_sticks();
    }
    emit ActiveChanged(on);
}

void StickCursor::Cancel() {
    held = false;
    SetActive(false);
}

void StickCursor::Place(const QPointF& point) {
    position = point;
    if (bounds.isValid()) {
        position.setX(std::clamp(position.x(), bounds.left(), bounds.right() - 1));
        position.setY(std::clamp(position.y(), bounds.top(), bounds.bottom() - 1));
    }
    emit Moved(false);
}

qreal StickCursor::Deflection() const {
    QPointF sum;
    for (const QPointF& stick : sticks) {
        sum += stick;
    }
    return std::hypot(sum.x(), sum.y());
}

void StickCursor::OnStick(int stick, float x, float y) {
    if (!active) {
        return;
    }
    // Up is positive for a stick and negative on a screen.
    sticks.insert(stick, QPointF(x, -y));
    if (!step_timer.isActive() && Deflection() >= DeadZone) {
        last_step = clock.elapsed();
        step_timer.start();
    }
}

/// Moves the cursor as the sticks say, frame by frame, for as long as one is pushed.
void StickCursor::Step() {
    if (!active) {
        step_timer.stop();
        return;
    }
    const qint64 now = clock.elapsed();
    const qreal seconds = static_cast<qreal>(std::clamp<qint64>(now - last_step, 0, MaxStepMs)) / 1000.0;
    last_step = now;
    QPointF sum;
    for (const QPointF& stick : std::as_const(sticks)) {
        sum += stick;
    }
    const qreal push = std::hypot(sum.x(), sum.y());
    if (push < DeadZone) {
        step_timer.stop();
        return;
    }
    // Slow near the centre, for the last few pixels onto a character; fast at the rim.
    const qreal full_speed = bounds.width() * speed_percent / 100.0 / CrossingSeconds;
    const qreal speed = std::pow(std::min(push, 1.0), Curve) * full_speed;
    QPointF next = position + QPointF(sum.x() / push * speed * seconds,
                                      sum.y() / push * speed * seconds);
    if (bounds.isValid()) {
        next.setX(std::clamp(next.x(), bounds.left(), bounds.right() - 1));
        next.setY(std::clamp(next.y(), bounds.top(), bounds.bottom() - 1));
    }
    if (next != position) {
        position = next;
        emit Moved(true);
    }
}

} // namespace ReaderUi
