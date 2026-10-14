// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>
#include "reader/core/input_filter.h"

namespace Reader {

InputFilter& InputFilter::Instance() {
    static InputFilter instance;
    return instance;
}

void InputFilter::SetHandlers(ButtonHandler button, StickHandler stick) {
    std::scoped_lock lock{mutex};
    button_handler = std::move(button);
    stick_handler = std::move(stick);
    if (!button_handler) {
        passed_on.clear();
    }
}

void InputFilter::SetClickButtons(std::vector<int> buttons, ClickHandler handler) {
    std::scoped_lock lock{mutex};
    click_handler = std::move(handler);
    clicks_down.clear();
    click_buttons = click_handler ? std::move(buttons) : std::vector<int>{};
    any_click.store(!click_buttons.empty(), std::memory_order_relaxed);
}

void InputFilter::SetPauseButton(int button, PauseHandler handler) {
    std::scoped_lock lock{mutex};
    pause_handler = std::move(handler);
    pause_button.store(pause_handler ? button : -1, std::memory_order_relaxed);
}

bool InputFilter::OnButton(const void* controller, std::size_t button, bool pressed) {
    // The statistics button: the reader's, press and release.
    if (const int pause = pause_button.load(std::memory_order_relaxed);
        pause >= 0 && button == static_cast<std::size_t>(pause)) [[unlikely]] {
        PauseHandler handler;
        {
            std::scoped_lock lock{mutex};
            handler = pause_handler;
        }
        if (pressed && handler) {
            handler();
        }
        return true;
    }
    // A click button: the reader's while the cursor has the sticks, and until it is let go.
    if (any_click.load(std::memory_order_relaxed)) [[unlikely]] {
        const int number = static_cast<int>(button);
        ClickHandler handler;
        bool is_click = false;
        {
            std::scoped_lock lock{mutex};
            for (const int click : click_buttons) {
                is_click |= click == number;
            }
            if (is_click) {
                if (pressed) {
                    if (!sticks_held.load(std::memory_order_relaxed)) {
                        return false;
                    }
                    clicks_down.insert(number);
                } else if (clicks_down.erase(number) == 0) {
                    return false;
                }
                handler = click_handler;
            }
        }
        if (is_click) {
            if (handler) {
                handler(pressed);
            }
            return true;
        }
    }
    const int wanted = watched.load(std::memory_order_relaxed);
    if (wanted < 0 || button != static_cast<std::size_t>(wanted)) [[likely]] {
        return false;
    }
    ButtonHandler handler;
    {
        std::scoped_lock lock{mutex};
        handler = button_handler;
    }
    const bool taken = handler && handler(pressed);
    std::scoped_lock lock{mutex};
    if (pressed) {
        if (!taken) {
            passed_on.insert(controller);
        }
        return taken;
    }
    // A game that saw the button go down has to see it come up, whoever has it by now.
    if (passed_on.erase(controller) != 0) {
        return false;
    }
    return taken;
}

bool InputFilter::OnStick(std::size_t stick, float x, float y) {
    if (!sticks_held.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    StickHandler handler;
    {
        std::scoped_lock lock{mutex};
        handler = stick_handler;
    }
    if (handler) {
        handler(stick, x, y);
    }
    return true;
}

} // namespace Reader
