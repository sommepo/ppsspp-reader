// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <format>
#include "reader/core/audio_tap.h"
#include "reader/core/engine.h"
#include "reader/core/lua_api.h"
#include "reader/core/text_codec.h"

// The reader has a Lua of its own, built as C++ (see reader/CMakeLists.txt), so no extern "C".
#include <lauxlib.h>
#include <lua.h>

namespace Reader {

namespace {

/// A handler that keeps failing is switched off so it cannot flood the log every frame.
constexpr u32 MaxConsecutiveFailures = 5;

/// Upper bound for a single guest memory read requested by a profile.
constexpr std::size_t MaxReadSize = 16 * 1024 * 1024;

} // Anonymous namespace

std::string TextEvent::ToJson() const {
    std::string out;
    out.reserve(160 + text.size() + speaker.size());
    out += std::format("{{\"seq\":{},\"frame\":{},\"audio\":{},\"channel\":\"", sequence, frame,
                       audio_position);
    AppendJsonString(out, channel);
    out += "\",\"speaker\":\"";
    AppendJsonString(out, speaker);
    out += "\",\"text\":\"";
    AppendJsonString(out, text);
    out += "\",\"ruby\":[";
    for (std::size_t i = 0; i < ruby.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += std::format("[{},{},\"", ruby[i].start, ruby[i].length);
        AppendJsonString(out, ruby[i].reading);
        out += "\"]";
    }
    out += "],\"id\":\"";
    AppendJsonString(out, source_id);
    out += "\",\"meta\":";
    out += meta_json.empty() ? "{}" : meta_json;
    if (!window.empty()) {
        out += ",\"window\":\"";
        AppendJsonString(out, window);
        out.push_back('"');
    }
    out.push_back('}');
    return out;
}

namespace {

/// Screen coordinates with at most two decimals, and none when they are whole.
void AppendCoordinate(std::string& out, float value) {
    const double rounded = std::round(static_cast<double>(value) * 100.0) / 100.0;
    out += std::format("{}", rounded == 0.0 ? 0.0 : rounded);
}

void AppendRect(std::string& out, const ScreenRect& rect) {
    out.push_back('[');
    AppendCoordinate(out, rect.x);
    out.push_back(',');
    AppendCoordinate(out, rect.y);
    out.push_back(',');
    AppendCoordinate(out, rect.width);
    out.push_back(',');
    AppendCoordinate(out, rect.height);
    out.push_back(']');
}

/// Layouts come from game memory by way of a script: keep nonsense away from frontends.
bool IsSane(const ScreenRect& rect) {
    const auto in_range = [](float v) { return std::isfinite(v) && v > -16384.0f && v < 16384.0f; };
    return in_range(rect.x) && in_range(rect.y) && in_range(rect.width) && in_range(rect.height) &&
           rect.width >= 0.0f && rect.height >= 0.0f;
}

bool IsSaneSpace(float value) {
    return std::isfinite(value) && value >= 1.0f && value <= 16384.0f;
}

} // Anonymous namespace

std::string TextLayout::ToJson() const {
    std::string out = "{\"window\":\"";
    AppendJsonString(out, window);
    out += std::format("\",\"seq\":{}", sequence);
    if (closed) {
        out += ",\"closed\":true}";
        return out;
    }
    out += ",\"space\":[";
    AppendCoordinate(out, space_width);
    out.push_back(',');
    AppendCoordinate(out, space_height);
    out.push_back(']');
    if (box) {
        out += ",\"box\":";
        AppendRect(out, *box);
    }
    out += ",\"glyphs\":[";
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        if (glyphs[i]) {
            AppendRect(out, *glyphs[i]);
        } else {
            out += "null";
        }
    }
    out += "]}";
    return out;
}

Engine& Engine::Instance() {
    static Engine instance;
    return instance;
}

Engine::Engine() = default;

Engine::~Engine() {
    if (lua) {
        LuaApi::CloseState(lua);
        lua = nullptr;
    }
}

void Engine::SetEventCallback(EventCallback callback) {
    std::scoped_lock lock{frontend_mutex};
    event_callback = std::move(callback);
}

void Engine::SetLayoutCallback(LayoutCallback callback) {
    std::scoped_lock lock{frontend_mutex};
    layout_callback = std::move(callback);
}

void Engine::SetLogCallback(LogCallback callback) {
    std::scoped_lock lock{frontend_mutex};
    log_callback = std::move(callback);
}

void Engine::SetDataProvider(DataProvider provider) {
    std::scoped_lock lock{frontend_mutex};
    data_provider = std::move(provider);
}

void Engine::Post(std::function<void()> fn) {
    std::scoped_lock lock{frontend_mutex};
    posted.push_back(std::move(fn));
}

void Engine::SetHookPredicate(HookPredicate predicate) {
    std::scoped_lock lock{frontend_mutex};
    hook_predicate = std::move(predicate);
}

bool Engine::ProfileWantsHooks(std::string_view source) {
    // Only the comments the file begins with are looked at.
    while (!source.empty()) {
        const std::size_t end = source.find('\n');
        std::string_view line = source.substr(0, end);
        source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
        const auto strip = [](std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            while (!text.empty() &&
                   (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
                text.remove_suffix(1);
            }
            return text;
        };
        line = strip(line);
        if (line.empty()) {
            continue;
        }
        if (!line.starts_with("--")) {
            break;
        }
        if (strip(line.substr(2)) == "reader: no hooks") {
            return false;
        }
    }
    return true;
}

bool Engine::WantsHooks(const std::string& game_id_) const {
    HookPredicate predicate;
    {
        std::scoped_lock lock{frontend_mutex};
        predicate = hook_predicate;
    }
    return predicate && predicate(game_id_);
}

void Engine::OnGameLoading(Host& host_) {
    std::scoped_lock lock{mutex};
    UnloadProfileLocked();
    host = &host_;
    modules.clear();
    loading_game = true;
    game_id.clear();
    frame = 0;
    next_sequence = 1;
    last_error.clear();
}

void Engine::OnModuleLoaded(const std::string& name, u64 base, u64 size,
                            const std::string& build_id) {
    std::scoped_lock lock{mutex};
    // Later ones belong to system applets. Tests, which run detached, name theirs when they like.
    if (!loading_game && !detached_memory) {
        return;
    }
    std::erase_if(modules, [&](const Module& module) { return module.name == name; });
    modules.push_back(Module{name, base, size, build_id});
}

void Engine::OnGameLoaded(const std::string& game_id_) {
    std::scoped_lock lock{mutex};
    loading_game = false;
    game_id = game_id_;
    for (const Module& module : modules) {
        Log(LogLevel::Info, std::format("module {} at {:X}, {:X} bytes, build {}", module.name,
                                        module.base, module.size, module.build_id));
    }
}

std::string Engine::GameId() const {
    std::scoped_lock lock{mutex};
    return game_id;
}

void Engine::OnShutdown() {
    {
        std::scoped_lock lock{mutex};
        UnloadProfileLocked();
        host = nullptr;
        modules.clear();
        loading_game = false;
        game_id.clear();
        picture = 0;
    }
    std::scoped_lock lock{frontend_mutex};
    posted.clear();
}

void Engine::OnStateLoaded() {
    std::scoped_lock lock{mutex};
    CloseAllLayouts();
    windows.clear();
    if (!lua) {
        return;
    }
    for (std::size_t i = 0; i < state_callbacks.size(); ++i) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, state_callbacks[i]);
        CallTop(0, "on_state_loaded");
    }
}

void Engine::OnFrame() {
    std::vector<std::function<void()>> work;
    {
        std::scoped_lock lock{frontend_mutex};
        work.swap(posted);
    }
    std::scoped_lock lock{mutex};
    ++frame;
    for (auto& fn : work) {
        fn();
    }

    if (!lua || frame_callbacks.empty()) {
        return;
    }
    // By index: a callback may register further callbacks.
    for (std::size_t i = 0; i < frame_callbacks.size(); ++i) {
        if (frame_callbacks[i].failures >= MaxConsecutiveFailures) {
            continue;
        }
        lua_rawgeti(lua, LUA_REGISTRYINDEX, frame_callbacks[i].lua_ref);
        lua_pushinteger(lua, static_cast<lua_Integer>(frame.load()));
        const bool ok = CallTop(1, "on_frame");
        FrameCallback& callback = frame_callbacks[i];
        if (ok) {
            callback.failures = 0;
        } else if (++callback.failures >= MaxConsecutiveFailures) {
            Log(LogLevel::Error,
                std::format("on_frame callback disabled after {} consecutive errors",
                            MaxConsecutiveFailures));
        }
    }
}

bool Engine::HasProfile() const {
    std::scoped_lock lock{mutex};
    return lua != nullptr;
}

std::string Engine::LastError() const {
    std::scoped_lock lock{mutex};
    return last_error;
}

std::size_t Engine::HookCount() const {
    std::scoped_lock lock{mutex};
    return hooks.size();
}

std::vector<Module> Engine::Modules() const {
    std::scoped_lock lock{mutex};
    return modules;
}

std::optional<Module> Engine::FindModule(const std::string& name) const {
    std::scoped_lock lock{mutex};
    for (const Module& module : modules) {
        if (module.name == name) {
            return module;
        }
    }
    return std::nullopt;
}

bool Engine::LoadProfile(const std::string& chunk_name, const std::string& source) {
    std::scoped_lock lock{mutex};
    return LoadProfileLocked(chunk_name, source);
}

bool Engine::LoadProfileLocked(const std::string& chunk_name, const std::string& source) {
    UnloadProfileLocked();
    last_error.clear();
    if (!detached_memory && !host) {
        last_error = "no game is running";
        return false;
    }

    lua = LuaApi::NewState(*this);
    if (!lua) {
        last_error = "out of memory creating the Lua state";
        return false;
    }
    hooks_allowed = ProfileWantsHooks(source);

    const std::string name = "@" + chunk_name;
    bool ok = luaL_loadbufferx(lua, source.data(), source.size(), name.c_str(), "t") == LUA_OK;
    if (ok) {
        LuaApi::ResetBudget(lua, LuaApi::Budget::Load);
        ok = lua_pcall(lua, 0, 0, 0) == LUA_OK;
    }
    if (!ok) {
        const char* message = lua_tostring(lua, -1);
        last_error = message ? message : "unknown Lua error";
        Log(LogLevel::Error, std::format("profile {} failed to load: {}", chunk_name, last_error));
        const std::string kept_error = last_error;
        UnloadProfileLocked();
        last_error = kept_error;
        return false;
    }

    Log(LogLevel::Info,
        std::format("profile {} loaded for {}: {} hooked address(es), {} frame callback(s)",
                    chunk_name, game_id, hooks.size(), frame_callbacks.size()));
    return true;
}

bool Engine::Execute(const std::string& code, std::string& output) {
    std::scoped_lock lock{mutex};
    output.clear();
    if (!lua && !LoadProfileLocked("interactive", "")) {
        output = last_error;
        return false;
    }
    const int base = lua_gettop(lua);
    // Try the chunk as an expression first so that `mem.u32(0x100000)` prints its value.
    const std::string expression = "return " + code;
    if (luaL_loadbufferx(lua, expression.data(), expression.size(), "=input", "t") != LUA_OK) {
        lua_settop(lua, base);
        if (luaL_loadbufferx(lua, code.data(), code.size(), "=input", "t") != LUA_OK) {
            const char* message = lua_tostring(lua, -1);
            output = message ? message : "unknown Lua error";
            lua_settop(lua, base);
            return false;
        }
    }
    lua_pushcfunction(lua, LuaApi::CallAndFormat);
    lua_insert(lua, -2);
    LuaApi::ResetBudget(lua, LuaApi::Budget::Load);
    const bool ok = lua_pcall(lua, 1, 1, 0) == LUA_OK;
    std::size_t size = 0;
    const char* text = lua_tolstring(lua, -1, &size);
    output = text ? std::string(text, size) : "unknown Lua error";
    lua_settop(lua, base);
    return ok;
}

void Engine::UnloadProfile() {
    std::scoped_lock lock{mutex};
    UnloadProfileLocked();
}

void Engine::UnloadProfileLocked() {
    CloseAllLayouts();
    windows.clear();
    // Profile hooks go; hooks with C++ handlers belong to whoever added them.
    bool had_hooks = false;
    for (auto it = hooks.begin(); it != hooks.end();) {
        auto& list = it->second;
        had_hooks |= std::erase_if(list, [](const Hook& hook) { return !hook.native; }) != 0;
        it = list.empty() ? hooks.erase(it) : std::next(it);
    }
    has_hooks.store(!hooks.empty(), std::memory_order_relaxed);
    frame_callbacks.clear();
    state_callbacks.clear();
    if (lua) {
        LuaApi::CloseState(lua);
        lua = nullptr;
    }
    if (had_hooks) {
        InvalidateCode();
    }
}

bool Engine::IsHookedSlow(u64 pc) const {
    std::scoped_lock lock{mutex};
    return hooks.contains(pc);
}

void Engine::Dispatch(const GuestRegisters& regs) {
    std::scoped_lock lock{mutex};
    const u64 pc = regs.pc;
    if (!hooks.contains(pc)) {
        return;
    }
    // A handler may register more hooks, which can rehash the map and reallocate this list, so
    // look the entry up again on every access instead of holding a reference across the call.
    for (std::size_t i = 0; i < hooks.at(pc).size(); ++i) {
        const Hook hook = hooks.at(pc)[i];
        if (hook.native) {
            hook.native(regs);
            if (!hooks.contains(pc)) {
                return; // the handler removed the hooks
            }
            continue;
        }
        if (!lua || hook.failures >= MaxConsecutiveFailures) {
            continue;
        }
        lua_pushcfunction(lua, LuaApi::CallHook);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, hook.lua_ref);
        lua_pushlightuserdata(lua, const_cast<GuestRegisters*>(&regs));
        const bool ok = CallTop(2, "hook");
        if (!hooks.contains(pc) || i >= hooks.at(pc).size()) {
            return; // the handler replaced the profile
        }
        Hook& after = hooks.at(pc)[i];
        if (ok) {
            after.failures = 0;
        } else if (++after.failures >= MaxConsecutiveFailures) {
            Log(LogLevel::Error, std::format("hook at {:X} disabled after {} consecutive errors",
                                             pc, MaxConsecutiveFailures));
        }
    }
}

bool Engine::CallTop(int arg_count, const char* what) {
    LuaApi::ResetBudget(lua);
    const int base = lua_gettop(lua) - arg_count;
    lua_pushcfunction(lua, LuaApi::Traceback);
    lua_insert(lua, base);
    const int status = lua_pcall(lua, arg_count, 0, base);
    if (status != LUA_OK) {
        const char* message = lua_tostring(lua, -1);
        last_error = message ? message : "unknown Lua error";
        Log(LogLevel::Error, std::format("{} handler failed: {}", what, last_error));
        lua_pop(lua, 1);
    }
    lua_remove(lua, base);
    return status == LUA_OK;
}

void Engine::InvalidateCode() const {
    // Code that was translated before a hook was added has no call for it, and code translated
    // for a hook that is gone still has one: drop everything. This only happens when a profile
    // is loaded or unloaded while the game runs.
    if (host) {
        host->InvalidateCode();
    }
}

std::string Engine::ReadMemory(u64 address, std::size_t size) const {
    if (size == 0) {
        return {};
    }
    size = std::min(size, MaxReadSize);
    std::scoped_lock lock{mutex};
    if (detached_memory) {
        return detached_memory(address, size);
    }
    if (!host) {
        return {};
    }
    return host->ReadMemory(address, size);
}

std::vector<MemoryRegion> Engine::MemoryRegions() const {
    std::scoped_lock lock{mutex};
    if (!host) {
        return {};
    }
    return host->MemoryRegions();
}

std::optional<u32> Engine::ReadU32(u64 address) const {
    const std::string bytes = ReadMemory(address, 4);
    if (bytes.size() != 4) {
        return std::nullopt;
    }
    const auto b = [&](int i) { return static_cast<u32>(static_cast<u8>(bytes[i])); };
    return b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
}

bool Engine::AddHook(u64 address, int lua_ref, std::string& error) {
    if (!hooks_allowed) {
        error = "this profile begins with \"-- reader: no hooks\", so its game may be running "
                "natively, where a hook is never reached";
        return false;
    }
    if ((address & 3) != 0) {
        error = std::format("address {:X} is not a multiple of four", address);
        return false;
    }
    if (!ReadU32(address)) {
        error = std::format("address {:X} is not mapped in the game", address);
        return false;
    }
    hooks[address].push_back(Hook{lua_ref, nullptr, 0});
    has_hooks.store(true, std::memory_order_relaxed);
    InvalidateCode();
    return true;
}

void Engine::AddNativeHook(u64 address, NativeHandler handler) {
    std::scoped_lock lock{mutex};
    hooks[address].push_back(Hook{NoLuaRef, std::move(handler), 0});
    has_hooks.store(true, std::memory_order_relaxed);
    InvalidateCode();
}

void Engine::ClearNativeHooks() {
    std::scoped_lock lock{mutex};
    bool removed = false;
    for (auto it = hooks.begin(); it != hooks.end();) {
        auto& list = it->second;
        removed |= std::erase_if(list, [](const Hook& hook) { return bool(hook.native); }) != 0;
        it = list.empty() ? hooks.erase(it) : std::next(it);
    }
    has_hooks.store(!hooks.empty(), std::memory_order_relaxed);
    if (removed) {
        InvalidateCode();
    }
}

void Engine::StartDetached(MemoryReader reader, const std::string& game_id_) {
    StopDetached();
    std::scoped_lock lock{mutex};
    if (host) {
        return; // detached operation is for use without an emulated system
    }
    detached_memory = std::move(reader);
    game_id = game_id_;
}

void Engine::StopDetached() {
    std::scoped_lock lock{mutex};
    if (host) {
        return;
    }
    UnloadProfileLocked();
    hooks.clear();
    has_hooks.store(false, std::memory_order_relaxed);
    detached_memory = nullptr;
    modules.clear();
    game_id.clear();
    frame = 0;
    picture = 0;
    next_sequence = 1;
    last_error.clear();
}

void Engine::AddStateCallback(int lua_ref) {
    state_callbacks.push_back(lua_ref);
}

void Engine::AddFrameCallback(int lua_ref) {
    frame_callbacks.push_back(FrameCallback{lua_ref, 0});
}

void Engine::Emit(TextEvent event) {
    event.text = SanitizeUtf8(event.text);
    if (event.text.empty()) {
        return;
    }
    event.speaker = SanitizeUtf8(event.speaker);
    event.channel = event.channel.empty() ? "dialogue" : SanitizeUtf8(event.channel);
    // Whoever receives the event may rely on ruby spans lying inside the text, in order and not
    // overlapping. A span that breaks this is dropped rather than the whole line.
    const std::size_t text_length = CountCodePoints(event.text);
    std::stable_sort(event.ruby.begin(), event.ruby.end(),
                     [](const auto& a, const auto& b) { return a.start < b.start; });
    std::vector<TextEvent::Ruby> spans;
    spans.reserve(event.ruby.size());
    u64 covered = 0;
    for (auto& ruby : event.ruby) {
        const u64 end = static_cast<u64>(ruby.start) + ruby.length;
        if (ruby.length == 0 || ruby.reading.empty() || ruby.start < covered || end > text_length) {
            Log(LogLevel::Warning,
                std::format("ruby span {}+{} dropped: outside the text or overlapping", ruby.start,
                            ruby.length));
            continue;
        }
        covered = end;
        ruby.reading = SanitizeUtf8(ruby.reading);
        spans.push_back(std::move(ruby));
    }
    event.ruby = std::move(spans);
    if (event.source_id.empty()) {
        const std::string key = event.channel + '\0' + event.speaker + '\0' + event.text;
        event.source_id =
            HashHex(std::span<const u8>{reinterpret_cast<const u8*>(key.data()), key.size()});
    } else {
        event.source_id = SanitizeUtf8(event.source_id);
    }
    event.sequence = next_sequence++;
    event.frame = frame;
    event.audio_position = AudioTap::Instance().Position();
    event.window = SanitizeUtf8(event.window);
    if (!event.window.empty()) {
        // New text in the window: a layout reported earlier was for the text it replaces.
        windows[event.window] = WindowText{event.sequence, text_length, false};
    }

    EventCallback callback;
    {
        std::scoped_lock lock{frontend_mutex};
        callback = event_callback;
    }
    if (callback) {
        callback(event);
    }
}

bool Engine::ShowLayout(TextLayout layout) {
    layout.window = SanitizeUtf8(layout.window);
    const auto it = windows.find(layout.window);
    if (it == windows.end()) {
        return false;
    }
    WindowText& known = it->second;
    layout.sequence = known.sequence;
    layout.closed = false;
    if (!IsSaneSpace(layout.space_width) || !IsSaneSpace(layout.space_height)) {
        return false;
    }
    if (layout.box && !IsSane(*layout.box)) {
        layout.box.reset();
    }
    // One entry per code point of the text, no more and no fewer.
    layout.glyphs.resize(known.code_points);
    for (auto& glyph : layout.glyphs) {
        if (glyph && !IsSane(*glyph)) {
            glyph.reset();
        }
    }
    known.shown = true;

    LayoutCallback callback;
    {
        std::scoped_lock lock{frontend_mutex};
        callback = layout_callback;
    }
    if (callback) {
        callback(layout);
    }
    return true;
}

void Engine::CloseLayout(const std::string& window_) {
    const std::string window = SanitizeUtf8(window_);
    const auto it = windows.find(window);
    if (it == windows.end() || !it->second.shown) {
        return;
    }
    it->second.shown = false;
    TextLayout layout;
    layout.window = window;
    layout.sequence = it->second.sequence;
    layout.closed = true;

    LayoutCallback callback;
    {
        std::scoped_lock lock{frontend_mutex};
        callback = layout_callback;
    }
    if (callback) {
        callback(layout);
    }
}

void Engine::CloseAllLayouts() {
    std::vector<std::string> shown;
    for (const auto& [window, text] : windows) {
        if (text.shown) {
            shown.push_back(window);
        }
    }
    for (const auto& window : shown) {
        CloseLayout(window);
    }
}

void Engine::Log(LogLevel level, const std::string& message) const {
    LogCallback callback;
    {
        std::scoped_lock lock{frontend_mutex};
        callback = log_callback;
    }
    if (callback) {
        callback(level, message);
    }
}

std::optional<std::string> Engine::LoadData(const std::string& name) const {
    DataProvider provider;
    {
        std::scoped_lock lock{frontend_mutex};
        provider = data_provider;
    }
    if (!provider) {
        return std::nullopt;
    }
    return provider(name);
}

} // namespace Reader
