// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Valve's Steam Controller (2026) without Steam: what its state reports mean, as a gamepad
// and a trackpad mouse. This is the reading half of the bridge that runs on the maintainer's
// Batocera box (sc_bridge.py), in C++ and free of any system: the reports come in, and what
// they amount to goes out to a Sink. The report layout is that of SDL's
// SDL_hidapi_steam_triton.c.
//
// The same file is in every reader fork.

#include <array>
#include <cstddef>
#include <cstdint>

namespace ReaderApp::SteamBridge {

/// The gamepad's buttons, numbered as SDL numbers a game controller's (2 and 3 alike).
enum Button : int {
    A, B, X, Y, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, ButtonCount
};
/// Its axes, in SDL's order. Sticks run from -32768 to 32767 (down and right positive),
/// triggers from 0 to 32767.
enum Axis : int { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, AxisCount };
enum class MouseButton { Left, Right };

/// Where what the controller does goes.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void PadButton(Button button, bool down) = 0;
    virtual void PadAxis(Axis axis, int value) = 0;
    /// The pointer moves by whole pixels (right and down positive).
    virtual void MouseMove(int dx, int dy) = 0;
    virtual void MouseClick(MouseButton button, bool down) = 0;
    /// The wheel turns: 120 is one notch, up positive.
    virtual void MouseWheel(int amount) = 0;
};

/// How the pads feel. The defaults are the Batocera bridge's.
struct Tuning {
    double swipe_pixels = 1100; ///< Pixels for one swipe across the whole right pad
    double acceleration = 1.0;  ///< Extra speed for fast swipes: 0 none, 1 doubles the fastest
    double smoothing = 0.6;     ///< 0..1: higher follows the finger more closely
    double scroll_notches = 14; ///< Wheel notches for one swipe down the whole left pad
    bool trackball = true;      ///< After a flick the pointer rolls on; a touch stops it
    double friction = 0.970;    ///< Share of the speed kept each report while rolling
    bool haptics = true;        ///< The pads tick under the finger
    double tick_pixels = 45;    ///< Pointer travel for one tick of the right pad
    int tick_micros = 900;      ///< A tick's strength: how long the pad's motor is driven
    int click_micros = 2600;    ///< The same for a pad pressed down (let go: half of it)
};

/// A tick for one of the pads' motors: the controller's output report MsgHapticPulse.
struct Pulse {
    int side;   ///< 0 the right pad, 1 the left
    int micros; ///< How long the motor is driven
};
/// That report: its ID, the side, then on time, off time and repeat count as 16-bit numbers.
constexpr std::uint8_t HapticReport = 0x81;
constexpr std::size_t HapticLength = 8;
inline std::array<std::uint8_t, HapticLength> HapticMessage(const Pulse& pulse) {
    const auto on = static_cast<std::uint16_t>(pulse.micros < 0 ? 0 : pulse.micros > 0xFFFF ? 0xFFFF : pulse.micros);
    return {HapticReport, static_cast<std::uint8_t>(pulse.side), static_cast<std::uint8_t>(on & 0xFF),
            static_cast<std::uint8_t>(on >> 8), 0, 0, 1, 0};
}

/// The report's ID, and how long a state report is at least.
constexpr std::uint8_t StateReport = 0x42;
constexpr std::size_t StateLength = 30;
/// The feature report that switches the controller's own keyboard-and-mouse emulation
/// ("lizard mode") off. The firmware switches it back on after a few seconds without it.
constexpr std::array<std::uint8_t, 6> LizardOff{1, 0x87, 3, 9, 0, 0};
constexpr std::size_t FeatureLength = 64;

/// The grips and the "..." button are the reader's own (reader_pad): their bits, for whoever
/// reads the same reports.
constexpr std::uint32_t Grip1 = 0x00020000, Grip2 = 0x00040000, Grip3 = 0x00000080,
                        Grip4 = 0x00000100, Dots = 0x00000010;

/// One controller: what was last said of it, so that only changes are passed on.
class Pad {
public:
    explicit Pad(Tuning tuning = {}) : tuning{tuning} {}

    /// How the pads feel from now on (the user moved a slider).
    void SetTuning(const Tuning& changed) {
        tuning = changed;
    }
    /// A report from the controller. Anything but a whole state report is ignored (false).
    bool Report(const std::uint8_t* data, std::size_t size, Sink& sink);
    /// The controller is gone: everything is let go.
    void Release(Sink& sink);
    /// A tick the last reports asked for, to be written to that controller; false when
    /// there is none left.
    bool TakePulse(Pulse& pulse) {
        if (pulse_count == 0) {
            return false;
        }
        pulse = pulses[--pulse_count];
        return true;
    }
    /// The buttons of the last report, as the controller sent them.
    [[nodiscard]] std::uint32_t Buttons() const {
        return buttons;
    }

private:
    bool Point(double px, double py, Sink& sink);
    void Tick(int side, int micros) {
        if (tuning.haptics && pulse_count < pulses.size()) {
            pulses[pulse_count++] = {side, micros};
        }
    }

    Tuning tuning;
    std::uint32_t buttons{};
    std::array<int, AxisCount> axes{};
    std::array<bool, AxisCount> axis_sent{};
    bool right_touched{};
    double right_x{}, right_y{};     ///< Smoothed right pad position while touched
    double velocity_x{}, velocity_y{}; ///< Pointer speed in pixels per report, smoothed
    double rest_x{}, rest_y{};       ///< Unsent fractions of a pixel
    bool rolling{};
    bool left_touched{};
    double left_y{};
    double wheel{};
    double wheel_notch{};            ///< Wheel turned since the last whole notch
    double travel{};                 ///< Pointer travel since the right pad's last tick
    int since_tick{};                ///< Reports since that tick
    std::array<Pulse, 4> pulses{};
    std::size_t pulse_count{};
};

} // namespace ReaderApp::SteamBridge
