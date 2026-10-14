// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

class QWidget;

namespace ReaderApp {
class ReaderSettings;
}

/**
 * The CRT picture's page of the reader's settings: a switch, a choice of ready-made looks
 * and a slider for everything the picture is made of. The same file in every reader fork,
 * and built from the table in reader/core/crt.h: nothing here names a slider.
 */
namespace ReaderCrtPage {

/// Under which the picture's values are kept in the reader's settings, as one line of text.
extern const char* const SettingsKey;

/// Hands the kept values to the renderer. At the app's start, and when the settings were
/// changed from outside the page.
void Load(const ReaderApp::ReaderSettings& settings);

struct Page {
    QWidget* widget = nullptr;
    /// Keeps what the page shows (the caller then syncs the settings).
    std::function<void()> save;
    /// Puts back what was there when the page was made: a change shows in the game's
    /// picture at once, so leaving without saving has something to undo.
    std::function<void()> revert;
};

[[nodiscard]] Page Make(ReaderApp::ReaderSettings& settings, QWidget* parent);

} // namespace ReaderCrtPage
