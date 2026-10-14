// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the emulator's own files call. Every call from an upstream file into the reader goes
// through this header, and carries a "Reader fork" comment where it is made.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Reader::Emu {

/// The emulator is about to start a game: forget the previous one.
void OnGameLoading();
/// An executable is about to be loaded as the running program (at boot, and again when a game
/// starts another executable of its disc).
void OnExecLoading();
/// One of the game's executables is in memory. `crc` is of the file as the game ships it.
void OnModuleLoaded(const char* name, uint32_t base, uint32_t size, uint32_t crc);
/// The program is loaded and about to run.
void OnExecLoaded();
void OnShutdown();
/// The game wrote a save to the memory stick (save sync sends it a moment later).
void OnSaveWritten();
/// A savestate was loaded: what the reader knew of the screen is no longer true.
void OnStateLoaded();
/// Once per frame of the display (60 times a second of the game's time).
void OnFrame();

/// A frontend's function that loads the profile of the game that was just loaded. Runs on the
/// emulator's thread, with the game's ID as its disc gives it ("ULJS00219").
void SetGameLoadedCallback(std::function<void(const std::string& game_id)> callback);

/// A frontend's function for when the game is gone.
void SetGameStoppedCallback(std::function<void()> callback);

/// One of the emulator's own menus asks for the reader ("menu": what the reader offers for
/// the running game; "settings": its own page). A frontend says how that is answered.
void SetMenuCallback(std::function<void(const std::string& what)> callback);
void OpenMenu(const std::string& what);

/// Whether the game of this ID has a text profile, for the emulator's game list. A frontend
/// that keeps profiles somewhere of its own says how that is known; without one the answer
/// is false and the caller looks in the emulator's assets.
void SetProfileCheck(std::function<bool(const std::string& game_id)> check);
bool HasProfileCheck();
bool HasProfile(const std::string& game_id);

extern std::atomic<bool> g_any_hooks;
bool IsHookedSlow(uint32_t pc);
/// True when the reader wants to be called before the instruction at `pc` runs.
inline bool IsHooked(uint32_t pc) {
    return g_any_hooks.load(std::memory_order_relaxed) && IsHookedSlow(pc);
}
/// The hooked instruction at `pc` is about to run; the CPU state is in currentMIPS.
void DispatchHook(uint32_t pc);

/// What the PSP is about to play: `frames` stereo frames of 32-bit samples at 44.1 kHz.
void OnAudio(const int32_t* samples, std::size_t frames);

/// A key or a controller's button went down or up, before the emulator maps it to anything
/// (key codes as the emulator numbers them, which is as Android does). True when the reader
/// keeps it for itself: its cursor's button, its clicks, its statistics button.
bool FilterKey(int device, int key_code, bool down);
/// Presses or releases a button of the PSP, by the number of its bit in the button word.
void PressButton(int bit, bool down);
/// Puts the game's stick at rest (for when the reader's cursor takes the sticks).
void RestSticks();
/// A stick moved. True when the game must see it at rest instead.
bool FilterStick(int stick, float x, float y);

} // namespace Reader::Emu
