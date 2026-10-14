// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Save sync as the emulator and its screens see it: the PSP's save folders on one side, the
// engine (engine.h, from the Eden Reader fork) on the other. Nothing here throws, and nothing
// here knows the emulator: the paths and the way to tell the user something are handed in.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SaveSync::Host {

struct Settings {
    bool enabled{};
    /// The service's addresses as the user wrote them: "host", "host:port", several with commas.
    std::string addresses;
    std::string code;
    /// What this device is called in the other devices' messages.
    std::string device;
};

/// A game whose save changed here and on another device: only the user knows which to keep.
struct Conflict {
    std::string title;
    std::string disc;
    std::string name;
    /// When each of the two was saved, in a sentence.
    std::string message;
};

/// A PSP game's ID ("ULJM05887") as the service names games: 16 hexadecimal digits, the four
/// letters' codes, three zeros and the five digits ("554C4A4D00005887"). Empty for what is
/// not a disc's ID (homebrew). DiscOf is the way back.
[[nodiscard]] std::string TitleOf(std::string_view disc);
[[nodiscard]] std::string DiscOf(std::string_view title);
/// Whether a folder of PSP/SAVEDATA belongs to the game: the ID, then what the game added.
[[nodiscard]] bool IsSaveFolderOf(std::string_view disc, std::string_view folder);
/// The games that have a save folder in `savedata`, by ID.
[[nodiscard]] std::vector<std::string> DiscsWithSaves(const std::string& savedata);

/**
 * Once, when the app starts. `savedata` is PSP/SAVEDATA, `data` a folder of save sync's own
 * (its settings, its notes, the saves that were replaced). `say` shows the user a sentence
 * and may be called from any thread. With the settings switched on, every game that has a
 * save here is brought in line in the background.
 */
void Start(const std::string& savedata, const std::string& data,
           std::function<void(const std::string&)> say, const std::string& device_name);
[[nodiscard]] bool Started();
/// Waits a moment for what is still to be sent, then ends.
void Shutdown();

[[nodiscard]] Settings Get();
void Set(const Settings& settings);

/// The game is loaded and about to run: its save is brought in line first (this waits for
/// the service, a few seconds at most when it is away). From then on the save is sent a
/// moment after each time the game wrote it.
void GameStarting(const std::string& disc, const std::string& name);
void GameStopped();
/// The game wrote a save.
void NoteSaved();
[[nodiscard]] std::string RunningDisc();

/// In the background, the outcome in Status: is the service there and the code right.
void Check();
/// In the background: every game with a save here.
void SyncAll();
/// In the background: answers a conflict. Taking the other device's save is refused while
/// the game runs.
void Resolve(const std::string& title, bool keep_mine);
/// In the background: this install's reading statistics (a JSON object with "days").
void SendStats(const std::string& install, const std::string& json);

/// The last thing there was to say.
[[nodiscard]] std::string Status();
[[nodiscard]] std::vector<Conflict> Conflicts();
/// Changes whenever Status or Conflicts did.
[[nodiscard]] std::uint64_t Generation();
/// Waits until the background is done (for tests). False when the time ran out.
bool Wait(int milliseconds);

} // namespace SaveSync::Host
