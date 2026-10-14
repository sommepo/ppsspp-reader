// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "reader/core/types.h"

struct lua_State;

namespace Reader {

/// Guest register snapshot taken at a hooked instruction: the PSP's MIPS registers. pc is the
/// address of the hooked instruction itself. r[4]-r[7] are a0-a3, the first four arguments of
/// a call; r[29] is sp, r[31] ra. f holds the bits of the floating-point registers.
struct GuestRegisters {
    std::array<u32, 32> r{};
    u32 pc{};
    u32 hi{};
    u32 lo{};
    std::array<u32, 32> f{};
};

struct MemoryRegion;

/**
 * What the engine needs of the emulator. The engine itself knows nothing of PPSSPP: the glue
 * that does is reader/core/emulator.cpp, and tests run the engine without any of it.
 */
class Host {
public:
    virtual ~Host() = default;
    /// Reads up to `size` bytes of the game's memory, stopping where it is not mapped.
    virtual std::string ReadMemory(u64 address, std::size_t size) = 0;
    /// Translated code is out of date: a hook was added or removed.
    virtual void InvalidateCode() = 0;
    virtual std::vector<MemoryRegion> MemoryRegions() = 0;
};

/// One piece of text presented by the game, as produced by a game profile.
struct TextEvent {
    struct Ruby {
        u32 start;  ///< Offset into `text`, in Unicode code points
        u32 length; ///< Length of the annotated base text, in Unicode code points
        std::string reading;
    };

    u64 sequence{};       ///< Increases by one for every event of the running game
    u64 frame{};          ///< Game frame on which the event was produced
    u64 audio_position{}; ///< AudioTap position when the event was produced (0 if capture is off)
    std::string channel;  ///< "dialogue" unless the profile says otherwise
    std::string speaker;
    std::string text; ///< Base text without ruby readings; lines separated by '\n'
    std::vector<Ruby> ruby;
    std::string source_id; ///< Stable identity of this text within the game
    std::string meta_json; ///< Flat JSON object with profile-defined extras, or empty
    std::string window;    ///< What the text is shown in, when the profile reports layouts

    /// Serialises the event as a single-line JSON object (valid UTF-8).
    [[nodiscard]] std::string ToJson() const;
};

/// A rectangle of the game's picture, in the units the profile says the picture measures
/// (see TextLayout::space_width).
struct ScreenRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

/**
 * Where the text of an event is in the game's picture: what a frontend needs to let the player
 * point at the game's own text. Belongs to the latest event of `window`; a later event for the
 * same window makes it obsolete, and `closed` says that the text is no longer shown.
 */
struct TextLayout {
    std::string window;
    u64 sequence{}; ///< Sequence number of the event the layout belongs to
    bool closed{};
    /// The size of the whole picture in the units of the rectangles. A game lays its screen out
    /// in units of its own (often 1920x1080 or 1280x720), whatever it is rendered at.
    float space_width{1280.0f};
    float space_height{720.0f};
    std::optional<ScreenRect> box; ///< The text box as a whole, if known
    /// One entry per Unicode code point of the event's text; empty where nothing is drawn.
    std::vector<std::optional<ScreenRect>> glyphs;

    /// Single-line JSON: {"window","seq","space":[w,h],"box":[x,y,w,h],
    /// "glyphs":[[x,y,w,h]|null,...]} or {"window","seq","closed":true}.
    [[nodiscard]] std::string ToJson() const;
};

/// A module of the running game: one of its executables as loaded into memory.
struct Module {
    std::string name;     ///< "main" for the game's own executable, else the module's name
    u64 base{};           ///< Where it was loaded
    u64 size{};
    std::string build_id; ///< Upper-case hex: a hash of the executable as the game ships it
};

/// A stretch of the game's address space that is mapped in one way throughout.
struct MemoryRegion {
    u64 base{};
    u64 size{};
    bool readable{};
    bool writable{};
    bool executable{};
    u32 state{}; ///< 1 the kernel's memory, 2 the game's (code, data and heap alike), 3 video memory
};

enum class LogLevel { Info, Warning, Error };

/**
 * Runs a per-game Lua profile that turns game state into text events.
 *
 * A profile registers handlers for guest code addresses ("hooks") and optional per-frame
 * callbacks. The CPU JIT calls back into the engine when a hooked instruction is about to run,
 * the handler reads guest memory and emits text, and the frontend receives the result as JSON.
 *
 * The engine is entered under a lock: every public member may be called from any thread (the
 * emulator's, a frontend's). Callbacks given to it run on the thread that caused them, with
 * the lock held, and must not call back into the engine.
 */
class Engine {
public:
    using EventCallback = std::function<void(const TextEvent& event)>;
    using LayoutCallback = std::function<void(const TextLayout& layout)>;
    using LogCallback = std::function<void(LogLevel level, const std::string& message)>;
    using DataProvider = std::function<std::optional<std::string>(const std::string& name)>;
    using NativeHandler = std::function<void(const GuestRegisters& regs)>;
    using HookPredicate = std::function<bool(const std::string& game_id)>;
    using MemoryReader = std::function<std::string(u64 address, std::size_t size)>;

    static Engine& Instance();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- Frontend configuration ----

    /// Receives every emitted event.
    void SetEventCallback(EventCallback callback);
    /// Receives on-screen positions of event text.
    void SetLayoutCallback(LayoutCallback callback);
    /// Receives profile diagnostics.
    void SetLogCallback(LogCallback callback);
    /// Supplies profile data files requested with reader.data(name).
    void SetDataProvider(DataProvider provider);
    /// Runs a function at the next frame boundary, under the engine's lock.
    void Post(std::function<void()> fn);
    /// Tells whether a game is going to get a profile. Asked while the game is loaded, because
    /// that is when the way its code is run is chosen, and hooks need the JIT (see WantsHooks).
    void SetHookPredicate(HookPredicate predicate);

    // ---- Emulation lifecycle ----

    /// A game is about to be loaded: forget the previous one.
    void OnGameLoading(Host& host);
    /// True when the game about to be loaded has to run on the JIT rather than natively: hooks
    /// are compiled into translated code, and code that runs natively is never translated.
    [[nodiscard]] bool WantsHooks(const std::string& game_id) const;
    /// Whether a profile's source asks for hooks. One that only reads memory from its frame
    /// callback says so with the line "-- reader: no hooks" among the comments it begins with,
    /// and its game can then run natively. reader.hook is an error in such a profile.
    [[nodiscard]] static bool ProfileWantsHooks(std::string_view source);
    /// The loader has put one of the game's executables into memory.
    void OnModuleLoaded(const std::string& name, u64 base, u64 size, const std::string& build_id);
    /// The game is loaded and about to run.
    void OnGameLoaded(const std::string& game_id);
    void OnShutdown();
    /// A savestate was loaded: every layout is withdrawn and the profile is told
    /// (reader.on_state_loaded), so that it can forget what it believed to be on screen.
    void OnStateLoaded();
    /// The game presented a frame.
    void OnFrame();
    /// The size, in pixels, of the picture the game hands over for display. A game draws a
    /// smaller picture in handheld mode than for a television, and lays its text out for it.
    void OnPicture(u32 width, u32 height) {
        picture.store((static_cast<u64>(width) << 32) | height, std::memory_order_relaxed);
    }
    /// Width and height of the game's picture, or zeros before it has shown one.
    [[nodiscard]] std::pair<u32, u32> Picture() const {
        const u64 value = picture.load(std::memory_order_relaxed);
        return {static_cast<u32>(value >> 32), static_cast<u32>(value)};
    }

    // ---- Profiles ----

    /// Replaces the running profile. Returns false and keeps no profile when the script fails.
    bool LoadProfile(const std::string& chunk_name, const std::string& source);
    void UnloadProfile();
    /// Runs a chunk in the profile's Lua state, starting an empty profile when none is loaded.
    /// `output` receives the returned values (tab separated) or the error. For development tools.
    bool Execute(const std::string& code, std::string& output);
    [[nodiscard]] bool HasProfile() const;
    [[nodiscard]] std::string LastError() const;
    /// The running game's ID as its disc or package gives it ("ULJS00219"), or empty.
    [[nodiscard]] std::string GameId() const;
    [[nodiscard]] u64 Frame() const {
        return frame.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t HookCount() const;
    /// The game's executables, in load order.
    [[nodiscard]] std::vector<Module> Modules() const;
    /// The module called `name`, or nothing.
    [[nodiscard]] std::optional<Module> FindModule(const std::string& name) const;

    // ---- CPU integration ----

    /// True when the instruction at `pc` of the game is hooked. Cheap when nothing is.
    [[nodiscard]] bool IsHooked(u64 pc) const {
        if (!has_hooks.load(std::memory_order_relaxed)) [[likely]] {
            return false;
        }
        return IsHookedSlow(pc);
    }
    /// Called by JIT-compiled code when a hooked instruction is about to execute.
    void Dispatch(const GuestRegisters& regs);

    /// Hooks a guest address with a C++ handler instead of a profile function.
    void AddNativeHook(u64 address, NativeHandler handler);
    void ClearNativeHooks();

    /**
     * Runs the engine without an emulated system, for tests of the engine, the JIT glue and
     * profiles: guest memory comes from `reader` and hooks apply to whatever code reaches
     * Dispatch. Starts from a clean state (no profile, no modules, frame 0, sequence 1).
     */
    void StartDetached(MemoryReader reader, const std::string& game_id = {});
    /// Leaves detached operation, removing the profile and every hook.
    void StopDetached();

    // ---- Guest memory for profiles and tools ----

    /// Reads up to `size` bytes, stopping at the first unmapped page.
    [[nodiscard]] std::string ReadMemory(u64 address, std::size_t size) const;
    [[nodiscard]] std::optional<u32> ReadU32(u64 address) const;
    /// What is mapped in the game's address space, in address order. For tools that search
    /// memory; a profile has no business scanning it while the game runs.
    [[nodiscard]] std::vector<MemoryRegion> MemoryRegions() const;

    // ---- Used by the Lua bindings (the lock is held) ----

    bool AddHook(u64 address, int lua_ref, std::string& error);
    bool hooks_allowed = true; ///< False while a profile that declared "no hooks" is loaded
    void AddFrameCallback(int lua_ref);
    void AddStateCallback(int lua_ref);
    void Emit(TextEvent event);
    /// Reports where the latest text of `layout.window` is on screen. Returns false when no
    /// text was emitted for that window (there is nothing the layout could belong to).
    bool ShowLayout(TextLayout layout);
    /// Reports that the text of a window is no longer on screen.
    void CloseLayout(const std::string& window);
    void Log(LogLevel level, const std::string& message) const;
    [[nodiscard]] std::optional<std::string> LoadData(const std::string& name) const;

private:
    Engine();
    ~Engine();

    struct Hook {
        int lua_ref;          ///< Registry reference of the profile function, or NoLuaRef
        NativeHandler native; ///< C++ handler, when there is no profile function
        u32 failures;
    };
    static constexpr int NoLuaRef = -2; // LUA_NOREF
    struct FrameCallback {
        int lua_ref;
        u32 failures;
    };

    [[nodiscard]] bool IsHookedSlow(u64 pc) const;
    void InvalidateCode() const;
    void UnloadProfileLocked();
    bool LoadProfileLocked(const std::string& chunk_name, const std::string& source);
    /// Calls the function below `arg_count` arguments on the Lua stack, reporting any error.
    bool CallTop(int arg_count, const char* what);

    /// Guards everything below except the atomics. Recursive: profile code calls back in.
    mutable std::recursive_mutex mutex;

    Host* host{};
    std::string game_id;
    std::atomic<u64> frame{};
    std::atomic<u64> picture{}; ///< Width in the upper half, height in the lower
    u64 next_sequence{1};
    std::vector<Module> modules;
    /// Between OnGameLoading and OnGameLoaded: the executables being loaded are the game's.
    /// Later ones belong to system applets and are of no interest.
    bool loading_game{};

    lua_State* lua{};
    std::unordered_map<u64, std::vector<Hook>> hooks;
    std::atomic<bool> has_hooks{false};
    std::vector<FrameCallback> frame_callbacks;
    std::vector<int> state_callbacks;

    /// What is known about a window that events were emitted for.
    struct WindowText {
        u64 sequence{};
        std::size_t code_points{};
        bool shown{}; ///< A layout was reported and not closed since
    };
    std::unordered_map<std::string, WindowText> windows;
    void CloseAllLayouts();
    std::string last_error;
    MemoryReader detached_memory; ///< Set while running detached from an emulated system

    mutable std::mutex frontend_mutex; ///< Guards the callbacks and the posted-work queue
    EventCallback event_callback;
    LayoutCallback layout_callback;
    LogCallback log_callback;
    DataProvider data_provider;
    HookPredicate hook_predicate;
    std::vector<std::function<void()>> posted;
};

} // namespace Reader
