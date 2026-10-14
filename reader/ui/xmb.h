// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The reader app's main screen: categories across, their entries below, in the manner of
// the PSP's own menu. It is built from the emulator's interface parts and replaces the
// contents of its main screen; the emulator's classic screen stays one entry away.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Common/File/Path.h"

namespace UI {
class ViewGroup;
}

namespace ReaderXmb {

/// What the main screen does for the entries.
struct Host {
	std::function<void(const Path &)> launch;
	std::function<void()> settings;
	std::function<void()> classic; ///< Show the emulator's own main screen instead
	std::function<void()> folder;  ///< Let the user choose a folder that has games
	std::function<void()> save_sync; ///< The page of save sync
	std::function<void()> exit;
};

/// True unless the user chose the emulator's own main screen.
bool Enabled();
void SetEnabled(bool enabled);

/// A folder the user chose for their games: looked in from now on.
void RememberFolder(const std::string &folder);

/// This month's colour of the ground (ABGR), for the emulator's other screens.
uint32_t GroundColor();

/// The screen's contents.
UI::ViewGroup *Build(const Host &host);

/// A game in the user's folders whose text can be read.
struct ReadableGame {
	std::string id, title, path;
};
/// Those games as far as they are known now: a game's ID is read from its file in the
/// background, so one that was just found is missing until it has been.
std::vector<ReadableGame> ReadableGames();

}  // namespace ReaderXmb
