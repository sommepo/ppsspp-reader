// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>
#include "reader/core/types.h"

namespace Reader {

/**
 * Lets a frontend take one controller button and the sticks away from the game: the reader's
 * cursor is held (or switched on) with a button and steered with the sticks.
 *
 * The emulated controllers ask the filter about every change of the button and of the sticks,
 * on whatever thread the input arrives; the handlers run there.
 */
class InputFilter {
public:
    /// The button went down or up. Returns true when the game must not hear of it.
    using ButtonHandler = std::function<bool(bool pressed)>;
    /// A stick is at (x, y), each from -1 to 1, right and up being positive.
    using StickHandler = std::function<void(std::size_t stick, float x, float y)>;
    /// One of the click buttons went down or up while the cursor had the sticks.
    using ClickHandler = std::function<void(bool pressed)>;

    static InputFilter& Instance();

    void SetHandlers(ButtonHandler button, StickHandler stick);

    /// The button to ask about (the emulator's key code for it: 104 for a controller's L2, as Android numbers keys), or a negative number for none.
    void SetButton(int button) {
        watched.store(button, std::memory_order_relaxed);
    }

    /**
     * Buttons (by their numbers) that click where the cursor
     * is while the cursor has the sticks, instead of reaching the game: for a frontend
     * without a trackpad to tap, where the stick that steers the cursor is pressed to click.
     * None unless a frontend sets them.
     */
    void SetClickButtons(std::vector<int> buttons, ClickHandler handler);

    /// Called when the statistics button goes down, on whatever thread the input arrives.
    using PauseHandler = std::function<void()>;

    /**
     * A second button of the reader's own (the emulator's key code for it: 104 for a controller's L2, as Android numbers keys, negative for none):
     * a press of it pauses or resumes the reading statistics. The game never hears of it.
     */
    void SetPauseButton(int button, PauseHandler handler);

    /// While set, the game sees the sticks at rest, wherever they are.
    void HoldSticks(bool held) {
        sticks_held.store(held, std::memory_order_relaxed);
    }
    [[nodiscard]] bool SticksHeld() const {
        return sticks_held.load(std::memory_order_relaxed);
    }

    // ---- Called by the emulated controllers ----

    /// Button `button` of `controller` changed. True when the game must not see the change.
    bool OnButton(const void* controller, std::size_t button, bool pressed);

    /// A stick of a controller moved. True when the game must see it at rest instead.
    bool OnStick(std::size_t stick, float x, float y);

private:
    InputFilter() = default;

    std::atomic<int> watched{-1};
    std::atomic<int> pause_button{-1};
    PauseHandler pause_handler;
    std::atomic<bool> sticks_held{false};
    std::mutex mutex;
    ButtonHandler button_handler;
    StickHandler stick_handler;
    /// Controllers that passed a press of the button on to the game: the release is theirs too.
    std::unordered_set<const void*> passed_on;
    std::atomic<bool> any_click{false};
    std::vector<int> click_buttons;
    ClickHandler click_handler;
    /// Click buttons whose press was taken as a click (a bit each): so is their release.
    std::unordered_set<int> clicks_down;
};

} // namespace Reader
