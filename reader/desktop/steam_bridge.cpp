// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "reader/desktop/steam_bridge.h"

namespace ReaderApp::SteamBridge {

namespace {

// The report: ID, a counter, 32 bits of buttons, the two triggers, the two sticks, the left
// pad (x, y, pressure), the right pad (x, y, pressure); all little-endian, 16 bits each
// after the buttons.
constexpr std::size_t ButtonsAt = 2, TriggerLeftAt = 6, TriggerRightAt = 8, LeftXAt = 10,
                      LeftYAt = 12, RightXAt = 14, RightYAt = 16, LeftPadYAt = 20,
                      RightPadXAt = 24, RightPadYAt = 26;

constexpr std::uint32_t DpadUpBit = 0x2000, DpadDownBit = 0x0400, DpadLeftBit = 0x1000,
                        DpadRightBit = 0x0800;
constexpr std::uint32_t RightPadTouch = 0x00200000, RightPadClick = 0x00400000,
                        LeftPadTouch = 0x02000000, LeftPadClick = 0x04000000;

struct ButtonBit {
    std::uint32_t bit;
    Button button;
};
constexpr ButtonBit ButtonBits[] = {
    {0x00000001, A},          {0x00000002, B},           {0x00000004, X},
    {0x00000008, Y},          {0x00080000, LeftShoulder}, {0x00000200, RightShoulder},
    {0x00000040, Back},       {0x00004000, Start},       {0x00010000, Guide},
    {0x00008000, LeftStick},  {0x00000020, RightStick},  {DpadUpBit, DpadUp},
    {DpadDownBit, DpadDown},  {DpadLeftBit, DpadLeft},   {DpadRightBit, DpadRight},
};

int Signed16(const std::uint8_t* data, std::size_t at) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8)));
}

} // namespace

bool Pad::Point(double px, double py, Sink& sink) {
    rest_x += px;
    rest_y += py;
    const int ix = static_cast<int>(rest_x);
    const int iy = static_cast<int>(rest_y);
    if (ix == 0 && iy == 0) {
        return false;
    }
    rest_x -= ix;
    rest_y -= iy;
    sink.MouseMove(ix, iy);
    // A tick in the pad now and then, as a trackball's bearings would give.
    travel += std::sqrt(static_cast<double>(ix * ix + iy * iy));
    if (travel >= std::max(tuning.tick_pixels, 5.0) && since_tick >= 3) {
        travel = 0;
        since_tick = 0;
        Tick(0, tuning.tick_micros);
    }
    return true;
}

bool Pad::Report(const std::uint8_t* data, std::size_t size, Sink& sink) {
    if (data == nullptr || size < StateLength || data[0] != StateReport) {
        return false;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(data[ButtonsAt]) |
                              (static_cast<std::uint32_t>(data[ButtonsAt + 1]) << 8) |
                              (static_cast<std::uint32_t>(data[ButtonsAt + 2]) << 16) |
                              (static_cast<std::uint32_t>(data[ButtonsAt + 3]) << 24);
    const std::uint32_t changed = now ^ buttons;
    for (const ButtonBit& entry : ButtonBits) {
        if ((changed & entry.bit) != 0) {
            sink.PadButton(entry.button, (now & entry.bit) != 0);
        }
    }
    // A resting stick wanders by a few units in every report: real movement only.
    const auto axis = [&](Axis which, int value, int slack) {
        if (!axis_sent[which] || std::abs(value - axes[which]) > slack) {
            axes[which] = value;
            axis_sent[which] = true;
            sink.PadAxis(which, value);
        }
    };
    axis(LeftX, Signed16(data, LeftXAt), 96);
    axis(LeftY, std::clamp(-Signed16(data, LeftYAt), -32768, 32767), 96);
    axis(RightX, Signed16(data, RightXAt), 96);
    axis(RightY, std::clamp(-Signed16(data, RightYAt), -32768, 32767), 96);
    axis(LeftTrigger, std::clamp(Signed16(data, TriggerLeftAt), 0, 32767), 0);
    axis(RightTrigger, std::clamp(Signed16(data, TriggerRightAt), 0, 32767), 0);

    // The pads pressed down are the mouse's buttons: the right pad the left one.
    // A pad has no switch under it to be felt: its motor gives the click.
    ++since_tick;
    if ((changed & RightPadClick) != 0) {
        const bool down = (now & RightPadClick) != 0;
        sink.MouseClick(MouseButton::Left, down);
        Tick(0, down ? tuning.click_micros : tuning.click_micros / 2);
    }
    if ((changed & LeftPadClick) != 0) {
        const bool down = (now & LeftPadClick) != 0;
        sink.MouseClick(MouseButton::Right, down);
        Tick(1, down ? tuning.click_micros : tuning.click_micros / 2);
    }

    // The right pad moves the pointer, as a trackball does.
    const double scale = tuning.swipe_pixels / 65536.0;
    const double alpha = std::clamp(tuning.smoothing, 0.05, 1.0);
    if ((now & RightPadTouch) != 0) {
        const double x = Signed16(data, RightPadXAt);
        const double y = Signed16(data, RightPadYAt);
        if (!right_touched) {
            right_touched = true;
            right_x = x;
            right_y = y;
            velocity_x = velocity_y = 0;
        } else {
            const double nx = right_x + (x - right_x) * alpha;
            const double ny = right_y + (y - right_y) * alpha;
            const double dx = nx - right_x;
            const double dy = ny - right_y;
            right_x = nx;
            right_y = ny;
            // Pad units per report; a brisk swipe is some hundreds.
            const double speed = std::sqrt(dx * dx + dy * dy);
            const double gain = scale * (1.0 + tuning.acceleration * std::min(speed / 600.0, 1.0));
            const double px = dx * gain;
            const double py = -dy * gain;
            velocity_x += (px - velocity_x) * 0.3;
            velocity_y += (py - velocity_y) * 0.3;
            Point(px, py, sink);
        }
        rolling = false;
    } else {
        if (right_touched) { // the finger has just left the pad
            rolling = tuning.trackball &&
                      std::sqrt(velocity_x * velocity_x + velocity_y * velocity_y) > 1.5;
        }
        right_touched = false;
        if (rolling) {
            const double friction = std::clamp(tuning.friction, 0.5, 0.999);
            velocity_x *= friction;
            velocity_y *= friction;
            if (std::fabs(velocity_x) < 0.4 && std::fabs(velocity_y) < 0.4) {
                rolling = false;
            } else {
                Point(velocity_x, velocity_y, sink);
            }
        }
        if (!rolling) {
            velocity_x = velocity_y = rest_x = rest_y = 0;
        }
    }

    // The left pad is the wheel.
    if ((now & LeftPadTouch) != 0) {
        const double y = Signed16(data, LeftPadYAt);
        if (left_touched) {
            const double turned = (y - left_y) * tuning.scroll_notches * 120.0 / 65536.0;
            wheel += turned;
            wheel_notch += turned;
            const int notches = static_cast<int>(wheel_notch / 120.0);
            if (notches != 0) {
                wheel_notch -= notches * 120.0;
                Tick(1, tuning.tick_micros);
            }
            const int whole = static_cast<int>(wheel);
            if (whole != 0) {
                wheel -= whole;
                sink.MouseWheel(whole);
            }
        }
        left_touched = true;
        left_y = y;
    } else {
        left_touched = false;
        wheel = 0;
        wheel_notch = 0;
    }
    buttons = now;
    return true;
}

void Pad::Release(Sink& sink) {
    for (const ButtonBit& entry : ButtonBits) {
        if ((buttons & entry.bit) != 0) {
            sink.PadButton(entry.button, false);
        }
    }
    if ((buttons & RightPadClick) != 0) {
        sink.MouseClick(MouseButton::Left, false);
    }
    if ((buttons & LeftPadClick) != 0) {
        sink.MouseClick(MouseButton::Right, false);
    }
    for (int which = 0; which < AxisCount; ++which) {
        if (axis_sent[which] && axes[which] != 0) {
            sink.PadAxis(static_cast<Axis>(which), 0);
        }
        axes[which] = 0;
    }
    buttons = 0;
    right_touched = left_touched = rolling = false;
    velocity_x = velocity_y = rest_x = rest_y = wheel = wheel_notch = travel = 0;
    pulse_count = 0;
}

} // namespace ReaderApp::SteamBridge
