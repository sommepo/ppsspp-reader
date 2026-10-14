// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The Steam Controller's reports as a gamepad and a trackpad mouse, with made-up reports.

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/steam_bridge.h"

using namespace ReaderApp::SteamBridge;

namespace {

struct Recorder final : Sink {
    std::vector<std::string> said;
    int moved_x{}, moved_y{}, wheel{};
    void PadButton(Button button, bool down) override {
        said.push_back("button " + std::to_string(button) + (down ? " down" : " up"));
    }
    void PadAxis(Axis axis, int value) override {
        said.push_back("axis " + std::to_string(axis) + " " + std::to_string(value));
    }
    void MouseMove(int dx, int dy) override {
        moved_x += dx;
        moved_y += dy;
    }
    void MouseClick(MouseButton button, bool down) override {
        said.push_back(std::string(button == MouseButton::Left ? "left" : "right") + (down ? " down" : " up"));
    }
    void MouseWheel(int amount) override {
        wheel += amount;
    }
};

struct Made {
    std::uint32_t buttons{};
    int trigger_left{}, trigger_right{}, lx{}, ly{}, rx{}, ry{}, left_pad_y{}, right_pad_x{}, right_pad_y{};
};

std::vector<std::uint8_t> Report(const Made& made) {
    std::vector<std::uint8_t> data(46, 0);
    data[0] = StateReport;
    const auto put16 = [&](std::size_t at, int value) {
        data[at] = static_cast<std::uint8_t>(value & 0xFF);
        data[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    };
    for (int i = 0; i < 4; ++i) {
        data[2 + i] = static_cast<std::uint8_t>((made.buttons >> (8 * i)) & 0xFF);
    }
    put16(6, made.trigger_left);
    put16(8, made.trigger_right);
    put16(10, made.lx);
    put16(12, made.ly);
    put16(14, made.rx);
    put16(16, made.ry);
    put16(20, made.left_pad_y);
    put16(24, made.right_pad_x);
    put16(26, made.right_pad_y);
    return data;
}

bool Feed(Pad& pad, Recorder& out, const Made& made) {
    const auto data = Report(made);
    return pad.Report(data.data(), data.size(), out);
}

} // namespace

TEST_CASE("SteamBridge: buttons and the d-pad are a game controller's", "[reader][steam]") {
    Pad pad;
    Recorder out;
    REQUIRE(Feed(pad, out, {}));
    out.said.clear();
    REQUIRE(Feed(pad, out, {.buttons = 0x1}));
    REQUIRE(out.said == std::vector<std::string>{"button 0 down"});
    out.said.clear();
    // A let go, Start and d-pad left down.
    REQUIRE(Feed(pad, out, {.buttons = 0x4000 | 0x1000}));
    REQUIRE(out.said == std::vector<std::string>{"button 0 up", "button 6 down", "button 13 down"});
    out.said.clear();
    // The grips and the "..." button are the reader's: no gamepad button is made of them.
    REQUIRE(Feed(pad, out, {.buttons = 0x4000 | 0x1000 | Grip1 | Grip4 | Dots}));
    REQUIRE(out.said.empty());
    REQUIRE((pad.Buttons() & Grip1) != 0);
    pad.Release(out);
    REQUIRE(out.said == std::vector<std::string>{"button 6 up", "button 13 up"});
}

TEST_CASE("SteamBridge: sticks and triggers, up being up", "[reader][steam]") {
    Pad pad;
    Recorder out;
    REQUIRE(Feed(pad, out, {}));
    out.said.clear();
    REQUIRE(Feed(pad, out, {.trigger_left = 32767, .lx = 20000, .ly = 30000}));
    // The controller counts a stick's "up" as positive; a game controller's axis counts down.
    REQUIRE(out.said == std::vector<std::string>{"axis 0 20000", "axis 1 -30000", "axis 4 32767"});
    out.said.clear();
    // A resting stick wanders by a few units: nothing is said of that.
    REQUIRE(Feed(pad, out, {.trigger_left = 32767, .lx = 20050, .ly = 30040}));
    REQUIRE(out.said.empty());
}

TEST_CASE("SteamBridge: the right pad moves the pointer and rolls on, a click is a click",
          "[reader][steam]") {
    Pad pad;
    Recorder out;
    const std::uint32_t touch = 0x00200000;
    REQUIRE(Feed(pad, out, {.buttons = touch, .right_pad_x = -8000, .right_pad_y = 0}));
    REQUIRE(out.moved_x == 0); // the first touch only says where the finger is
    for (int x = -7000; x <= 8000; x += 1000) {
        REQUIRE(Feed(pad, out, {.buttons = touch, .right_pad_x = x, .right_pad_y = 0}));
    }
    // A swipe to the right over a quarter of the pad: to the right on screen, and no way up.
    REQUIRE(out.moved_x > 150);
    REQUIRE(out.moved_x < 1100);
    REQUIRE(out.moved_y == 0);
    // Up on the pad is up on the screen.
    Recorder up;
    Pad other;
    for (int y = 0; y <= 8000; y += 1000) {
        REQUIRE(Feed(other, up, {.buttons = touch, .right_pad_x = 0, .right_pad_y = y}));
    }
    REQUIRE(up.moved_y < -50);
    // Let go in mid-swipe, it rolls on for a while and comes to rest.
    const int at_release = out.moved_x;
    int reports = 0;
    int last = at_release;
    for (; reports < 2000; ++reports) {
        REQUIRE(Feed(pad, out, {}));
        if (reports > 10 && out.moved_x == last && reports % 50 == 0) {
            break;
        }
        if (reports % 50 == 0) {
            last = out.moved_x;
        }
    }
    REQUIRE(out.moved_x > at_release);
    REQUIRE(reports < 2000);
    // A touch stops it.
    Pad flick;
    Recorder rolled;
    for (int x = -8000; x <= 8000; x += 2000) {
        REQUIRE(Feed(flick, rolled, {.buttons = touch, .right_pad_x = x}));
    }
    REQUIRE(Feed(flick, rolled, {}));
    REQUIRE(Feed(flick, rolled, {.buttons = touch, .right_pad_x = 0}));
    const int stopped = rolled.moved_x;
    REQUIRE(Feed(flick, rolled, {.buttons = touch, .right_pad_x = 0}));
    REQUIRE(rolled.moved_x == stopped);

    out.said.clear();
    REQUIRE(Feed(pad, out, {.buttons = 0x00200000 | 0x00400000}));
    REQUIRE(Feed(pad, out, {.buttons = 0x00200000}));
    REQUIRE(Feed(pad, out, {.buttons = 0x04000000}));
    REQUIRE(out.said == std::vector<std::string>{"left down", "left up", "right down"});
}

TEST_CASE("SteamBridge: the left pad is the wheel", "[reader][steam]") {
    Pad pad;
    Recorder out;
    const std::uint32_t touch = 0x02000000;
    for (int y = -16000; y <= 16000; y += 2000) {
        REQUIRE(Feed(pad, out, {.buttons = touch, .left_pad_y = y}));
    }
    // Half the pad's height, upwards: seven notches up.
    REQUIRE(out.wheel > 6 * 120);
    REQUIRE(out.wheel < 8 * 120);
}

TEST_CASE("SteamBridge: only whole state reports count", "[reader][steam]") {
    Pad pad;
    Recorder out;
    auto data = Report({.buttons = 1});
    REQUIRE_FALSE(pad.Report(data.data(), 10, out));
    data[0] = 0x79;
    REQUIRE_FALSE(pad.Report(data.data(), data.size(), out));
    REQUIRE_FALSE(pad.Report(nullptr, 0, out));
    REQUIRE(out.said.empty());
}

TEST_CASE("SteamBridge: the pads tick under the finger and click when pressed", "[reader][steam]") {
    Pad pad;
    Recorder out;
    const auto pulses = [&] {
        std::vector<Pulse> all;
        Pulse pulse{};
        while (pad.TakePulse(pulse)) {
            all.push_back(pulse);
        }
        return all;
    };
    // A swipe across the right pad: ticks of the right motor, none of the left.
    int ticks = 0;
    for (int step = 0; step <= 40; ++step) {
        REQUIRE(Feed(pad, out, {.buttons = 0x00200000, .right_pad_x = -20000 + step * 1000}));
        for (const Pulse& pulse : pulses()) {
            REQUIRE(pulse.side == 0);
            REQUIRE(pulse.micros == 900);
            ++ticks;
        }
    }
    REQUIRE(out.moved_x > 300);
    REQUIRE(ticks >= 3);
    REQUIRE(ticks <= out.moved_x / 45 + 1);
    pad.Release(out);

    // The right pad pressed and let go: a click, and half a click.
    REQUIRE(Feed(pad, out, {.buttons = 0x00200000 | 0x00400000}));
    auto felt = pulses();
    REQUIRE(felt.size() == 1);
    REQUIRE(felt[0].side == 0);
    REQUIRE(felt[0].micros == 2600);
    REQUIRE(Feed(pad, out, {.buttons = 0x00200000}));
    felt = pulses();
    REQUIRE(felt.size() == 1);
    REQUIRE(felt[0].micros == 1300);
    // The left pad pressed: the left motor.
    REQUIRE(Feed(pad, out, {.buttons = 0x04000000}));
    felt = pulses();
    REQUIRE(felt.size() == 1);
    REQUIRE(felt[0].side == 1);
    pad.Release(out);

    // The wheel: a tick of the left motor for each whole notch.
    out.wheel = 0;
    int notches = 0;
    for (int step = 0; step <= 20; ++step) {
        REQUIRE(Feed(pad, out, {.buttons = 0x02000000, .left_pad_y = step * 1000}));
        for (const Pulse& pulse : pulses()) {
            REQUIRE(pulse.side == 1);
            ++notches;
        }
    }
    REQUIRE(notches == std::abs(out.wheel) / 120);
    REQUIRE(notches >= 3);

    // The report that goes to the controller.
    const auto message = HapticMessage({1, 900});
    REQUIRE(message == std::array<std::uint8_t, 8>{0x81, 1, 0x84, 0x03, 0, 0, 1, 0});

    // And none of it with haptics off.
    Tuning quiet;
    quiet.haptics = false;
    Pad silent{quiet};
    REQUIRE(Feed(silent, out, {.buttons = 0x00200000 | 0x00400000}));
    Pulse none{};
    REQUIRE_FALSE(silent.TakePulse(none));
}
