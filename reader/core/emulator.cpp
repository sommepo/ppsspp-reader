// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/System.h"

#include "reader/core/audio_tap.h"
#include "reader/core/emulator.h"
#include "reader/core/engine.h"
#include "reader/core/input_filter.h"
#include "reader/save_sync/host.h"

namespace Reader::Emu {

std::atomic<bool> g_any_hooks{false};

namespace {

class PspHost final : public Host {
public:
    std::string ReadMemory(u64 address, std::size_t size) override {
        if (address > 0xFFFFFFFFULL || !Memory::base) {
            return {};
        }
        const u32 start = static_cast<u32>(address);
        const u32 want = static_cast<u32>(std::min<std::size_t>(size, 0x7FFFFFFF));
        const u32 valid = Memory::ClampValidSizeAt(start, want);
        if (valid == 0) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(Memory::GetPointerUnchecked(start)),
                           valid);
    }

    void InvalidateCode() override {
        g_any_hooks.store(Engine::Instance().HookCount() != 0, std::memory_order_relaxed);
        // Safe from inside translated code too: the emulator defers the clearing then.
        if (PSP_IsInited() || PSP_GetBootState() == BootState::Booting) {
            mipsr4k.ClearJitCache();
        }
    }

    std::vector<MemoryRegion> MemoryRegions() override {
        if (!Memory::base) {
            return {};
        }
        const u32 user_end = PSP_GetUserMemoryEnd();
        return {
            MemoryRegion{0x00010000, 0x00004000, true, true, false, 4},
            MemoryRegion{0x04000000, 0x00200000, true, true, false, 3},
            MemoryRegion{0x08000000, 0x00800000, true, true, true, 1},
            MemoryRegion{0x08800000, user_end - 0x08800000, true, true, true, 2},
        };
    }
};

PspHost g_host;
std::mutex g_callback_mutex;
std::function<void(const std::string&)> g_game_loaded;
std::function<void()> g_game_stopped;
std::function<void(const std::string&)> g_menu;
bool g_has_main = false;
std::vector<s16> g_audio_scratch;

} // Anonymous namespace

void OnGameLoading() {
    Engine::Instance().OnGameLoading(g_host);
    g_has_main = false;
}

void OnExecLoading() {
    Engine::Instance().OnGameLoading(g_host);
    g_has_main = false;
}

void OnModuleLoaded(const char* name, uint32_t base, uint32_t size, uint32_t crc) {
    // The first executable of a program is the game's own; it is "main" to profiles, whatever
    // it calls itself. Modules it loads later go by their names.
    char build[16];
    std::snprintf(build, sizeof(build), "%08X", crc);
    if (!g_has_main) {
        g_has_main = true;
        Engine::Instance().OnModuleLoaded("main", base, size, build);
        return;
    }
    Engine::Instance().OnModuleLoaded(name ? name : "", base, size, build);
}

void OnExecLoaded() {
    const std::string game_id = g_paramSFO.GetDiscID();
#ifndef READER_NO_SAVE_SYNC
    // Before the game reads its save: another device's newer one takes its place.
    SaveSync::Host::GameStarting(game_id, g_paramSFO.GetValueString("TITLE"));
#endif
    Engine& engine = Engine::Instance();
    engine.OnGameLoaded(game_id);
    engine.OnPicture(480, 272);
    std::function<void(const std::string&)> callback;
    {
        std::scoped_lock lock{g_callback_mutex};
        callback = g_game_loaded;
    }
    if (callback) {
        callback(game_id);
    }
    g_any_hooks.store(engine.HookCount() != 0, std::memory_order_relaxed);
}

void OnSaveWritten() {
#ifndef READER_NO_SAVE_SYNC
    SaveSync::Host::NoteSaved();
#endif
}

void OnShutdown() {
#ifndef READER_NO_SAVE_SYNC
    SaveSync::Host::GameStopped();
#endif
    Engine::Instance().OnShutdown();
    g_any_hooks.store(false, std::memory_order_relaxed);
    g_has_main = false;
    InputFilter::Instance().HoldSticks(false);
    std::function<void()> callback;
    {
        std::scoped_lock lock{g_callback_mutex};
        callback = g_game_stopped;
    }
    if (callback) {
        callback();
    }
}

void SetGameStoppedCallback(std::function<void()> callback) {
    std::scoped_lock lock{g_callback_mutex};
    g_game_stopped = std::move(callback);
}

void SetMenuCallback(std::function<void(const std::string& what)> callback) {
    std::scoped_lock lock{g_callback_mutex};
    g_menu = std::move(callback);
}

namespace {
std::function<bool(const std::string&)> g_profile_check;
}

void SetProfileCheck(std::function<bool(const std::string&)> check) {
    std::scoped_lock lock{g_callback_mutex};
    g_profile_check = std::move(check);
}

bool HasProfileCheck() {
    std::scoped_lock lock{g_callback_mutex};
    return static_cast<bool>(g_profile_check);
}

bool HasProfile(const std::string& game_id) {
    std::function<bool(const std::string&)> check;
    {
        std::scoped_lock lock{g_callback_mutex};
        check = g_profile_check;
    }
    return check && check(game_id);
}

void OpenMenu(const std::string& what) {
    std::function<void(const std::string&)> callback;
    {
        std::scoped_lock lock{g_callback_mutex};
        callback = g_menu;
    }
    if (callback) {
        callback(what);
    }
}

void OnStateLoaded() {
    Engine::Instance().OnStateLoaded();
}

void OnFrame() {
    Engine::Instance().OnFrame();
}

void SetGameLoadedCallback(std::function<void(const std::string& game_id)> callback) {
    std::scoped_lock lock{g_callback_mutex};
    g_game_loaded = std::move(callback);
}

bool IsHookedSlow(uint32_t pc) {
    return Engine::Instance().IsHooked(pc);
}

void DispatchHook(uint32_t pc) {
    const MIPSState& cpu = *currentMIPS;
    GuestRegisters regs;
    for (std::size_t i = 0; i < 32; ++i) {
        regs.r[i] = cpu.r[i];
        regs.f[i] = static_cast<u32>(cpu.fi[i]);
    }
    regs.pc = pc;
    regs.hi = cpu.hi;
    regs.lo = cpu.lo;
    Engine::Instance().Dispatch(regs);
}

void OnAudio(const int32_t* samples, std::size_t frames) {
    AudioTap& tap = AudioTap::Instance();
    if (!tap.IsEnabled()) {
        return;
    }
    g_audio_scratch.resize(frames * 2);
    for (std::size_t i = 0; i < frames * 2; ++i) {
        g_audio_scratch[i] = static_cast<s16>(std::clamp<int32_t>(samples[i], -32768, 32767));
    }
    tap.Push(g_audio_scratch.data(), frames);
}

bool FilterKey(int device, int key_code, bool down) {
    if (key_code < 0) {
        return false;
    }
    // The device stands for "the controller" the filter remembers a passed-on press by.
    return InputFilter::Instance().OnButton(
        reinterpret_cast<const void*>(static_cast<std::intptr_t>(device) + 1),
        static_cast<std::size_t>(key_code), down);
}

void PressButton(int bit, bool down) {
    if (!PSP_IsInited() || bit < 0 || bit > 31) {
        return;
    }
    const u32 mask = 1u << bit;
    __CtrlUpdateButtons(down ? mask : 0, down ? 0 : mask);
}

void RestSticks() {
    if (PSP_IsInited()) {
        __CtrlSetAnalogXY(0, 0.0f, 0.0f);
    }
}

bool FilterStick(int stick, float x, float y) {
    return InputFilter::Instance().OnStick(static_cast<std::size_t>(stick), x, y);
}

} // namespace Reader::Emu
