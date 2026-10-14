// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <format>
#include "reader/core/engine.h"
#include "reader/core/lua_api.h"
#include "reader/core/text_codec.h"

// The reader has a Lua of its own, built as C++ (see reader/CMakeLists.txt), so no extern "C". Lua errors
// are therefore C++ exceptions and unwind through the bindings below safely.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

namespace Reader::LuaApi {

namespace {

/// The count hook fires every this many VM instructions...
constexpr int BudgetStep = 10000;
/// ...and a callback may use this many steps (10 million instructions) before it is aborted. A
/// callback runs inside a frame of the game, so this is already far beyond what one can afford;
/// the limit only exists to stop a runaway loop from freezing emulation.
constexpr std::intptr_t CallbackBudgetSteps = 1000;
/// Loading a profile may build tables from data files: 500 million instructions.
constexpr std::intptr_t LoadBudgetSteps = 50000;

constexpr lua_Integer MaxStringRead = 1 << 20;

/// Memory a profile's Lua state may hold. Far more than a profile with data tables needs; it
/// turns a runaway allocation into a Lua error instead of the system killing the emulator.
constexpr std::size_t MaxStateMemory = 128 * 1024 * 1024;

/// Per-state bookkeeping, shared by the main thread and every coroutine of a state.
struct StateData {
    std::size_t used{};     ///< Bytes currently allocated
    std::intptr_t budget{}; ///< Count-hook steps left for the running callback
};

void* LimitedAlloc(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto* account = static_cast<StateData*>(ud);
    // For a new block Lua passes the kind of object in old_size, not a size.
    const std::size_t held = ptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        account->used -= held;
        return nullptr;
    }
    if (new_size > held && account->used - held + new_size > MaxStateMemory) {
        return nullptr;
    }
    void* block = std::realloc(ptr, new_size);
    if (block) {
        account->used = account->used - held + new_size;
    }
    return block;
}

std::intptr_t& BudgetLeft(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<StateData*>(ud)->budget;
}

const char EngineKey = 0;
const char CpuKey = 0;

/// The `cpu` argument of a hook handler: the registers of the dispatch in progress. One object
/// per state, refilled for every dispatch, so that a hook costs no allocation and no table.
struct CpuView {
    GuestRegisters regs;
    bool live;
};

/// Parses a register number: decimal digits only, at most `max`.
bool RegisterNumber(const char* text, std::size_t length, unsigned max, unsigned& out) {
    if (length == 0 || length > 2) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    out = value;
    return value <= max && (length == 1 || text[0] != '0');
}

int CpuIndex(lua_State* L) {
    const auto* view = static_cast<const CpuView*>(lua_touserdata(L, 1));
    std::size_t length = 0;
    const char* key = lua_tolstring(L, 2, &length);
    if (!view || !key) {
        lua_pushnil(L);
        return 1;
    }
    if (!view->live) {
        return luaL_error(L, "cpu is only valid inside its hook handler; copy what you need");
    }
    const GuestRegisters& regs = view->regs;
    const std::string_view name{key, length};
    // The registers by their usual names, as a disassembler prints them.
    static constexpr std::array<std::string_view, 32> Names{
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    unsigned n = 0;
    if (name == "pc") {
        lua_pushinteger(L, static_cast<lua_Integer>(regs.pc));
    } else if (name == "hi") {
        lua_pushinteger(L, static_cast<lua_Integer>(regs.hi));
    } else if (name == "lo") {
        lua_pushinteger(L, static_cast<lua_Integer>(regs.lo));
    } else if (key[0] == 'r' && RegisterNumber(key + 1, length - 1, 31, n)) {
        lua_pushinteger(L, static_cast<lua_Integer>(regs.r[n]));
    } else if (key[0] == 'f' && RegisterNumber(key + 1, length - 1, 31, n)) {
        float value;
        std::memcpy(&value, &regs.f[n], sizeof(value));
        lua_pushnumber(L, static_cast<lua_Number>(value));
    } else {
        bool found = false;
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (name == Names[i]) {
                lua_pushinteger(L, static_cast<lua_Integer>(regs.r[i]));
                found = true;
                break;
            }
        }
        if (!found) {
            lua_pushnil(L);
        }
    }
    return 1;
}

Engine& GetEngine(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &EngineKey);
    auto* engine = static_cast<Engine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *engine;
}

void BudgetHook(lua_State* L, lua_Debug*) {
    std::intptr_t& budget = BudgetLeft(L);
    if (budget > 0 && --budget > 0) {
        return;
    }
    // Spent: every further step fails too, so that a pcall inside the profile cannot keep the
    // callback going (see the pcall wrappers in the prelude).
    luaL_error(L, "profile callback exceeded its instruction budget (endless loop?)");
}

/// True once the running callback has used up its budget.
int BudgetExhausted(lua_State* L) {
    lua_pushboolean(L, BudgetLeft(L) <= 0);
    return 1;
}

u64 CheckAddress(lua_State* L, int index) {
    // Addresses are 64-bit and Lua's integers are signed 64-bit: the bits are what counts.
    return static_cast<u64>(luaL_checkinteger(L, index));
}

std::span<const u8> CheckBytes(lua_State* L, int index) {
    std::size_t size = 0;
    const char* data = luaL_checklstring(L, index, &size);
    return {reinterpret_cast<const u8*>(data), size};
}

void PushString(lua_State* L, const std::string& s) {
    lua_pushlstring(L, s.data(), s.size());
}

std::string FieldString(lua_State* L, int table, const char* name) {
    std::string out;
    lua_getfield(L, table, name);
    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
        std::size_t size = 0;
        const char* data = lua_tolstring(L, -1, &size);
        out.assign(data, size);
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "emit: field '%s' must be a string", name);
    }
    lua_pop(L, 1);
    return out;
}

// ---- reader ----

int ReaderHook(lua_State* L) {
    const u64 address = CheckAddress(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    std::string error;
    if (!GetEngine(L).AddHook(address, ref, error)) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "hook: %s", error.c_str());
    }
    return 0;
}

int ReaderOnFrame(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    GetEngine(L).AddFrameCallback(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int ReaderOnStateLoaded(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    GetEngine(L).AddStateCallback(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

/// reader.module(name): {base, size, build_id} of one of the game's executables, or nil.
int ReaderModule(lua_State* L) {
    const std::string name = luaL_optstring(L, 1, "main");
    const auto module = GetEngine(L).FindModule(name);
    if (!module) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, static_cast<lua_Integer>(module->base));
    lua_setfield(L, -2, "base");
    lua_pushinteger(L, static_cast<lua_Integer>(module->size));
    lua_setfield(L, -2, "size");
    lua_pushlstring(L, module->build_id.data(), module->build_id.size());
    lua_setfield(L, -2, "build_id");
    return 1;
}

int ReaderEmit(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    TextEvent event;
    event.text = FieldString(L, 1, "text");
    event.speaker = FieldString(L, 1, "speaker");
    event.channel = FieldString(L, 1, "channel");
    event.source_id = FieldString(L, 1, "id");
    event.window = FieldString(L, 1, "window");

    lua_getfield(L, 1, "ruby");
    if (lua_istable(L, -1)) {
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                return luaL_error(L, "emit: ruby[%d] must be {start, length, reading}",
                                  static_cast<int>(i));
            }
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            lua_rawgeti(L, -3, 3);
            const lua_Integer start = lua_tointeger(L, -3);
            const lua_Integer length = lua_tointeger(L, -2);
            std::size_t size = 0;
            const char* reading = lua_tolstring(L, -1, &size);
            if (start < 0 || length <= 0 || !reading) {
                return luaL_error(L, "emit: ruby[%d] must be {start, length, reading}",
                                  static_cast<int>(i));
            }
            event.ruby.push_back(TextEvent::Ruby{static_cast<u32>(start), static_cast<u32>(length),
                                                 std::string(reading, size)});
            lua_pop(L, 4);
        }
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "emit: field 'ruby' must be a table");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "meta");
    if (lua_istable(L, -1)) {
        std::map<std::string, std::string> entries; // sorted for stable output
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::string value;
                switch (lua_type(L, -1)) {
                case LUA_TBOOLEAN:
                    value = lua_toboolean(L, -1) ? "true" : "false";
                    break;
                case LUA_TNUMBER:
                    if (lua_isinteger(L, -1)) {
                        value = std::format("{}", static_cast<s64>(lua_tointeger(L, -1)));
                    } else {
                        const double number = lua_tonumber(L, -1);
                        value = std::isfinite(number) ? std::format("{}", number) : "null";
                    }
                    break;
                case LUA_TSTRING: {
                    std::size_t size = 0;
                    const char* data = lua_tolstring(L, -1, &size);
                    value = "\"";
                    AppendJsonString(value, SanitizeUtf8(std::string_view(data, size)));
                    value.push_back('"');
                    break;
                }
                default:
                    break;
                }
                if (!value.empty()) {
                    entries.emplace(SanitizeUtf8(lua_tostring(L, -2)), std::move(value));
                }
            }
            lua_pop(L, 1);
        }
        if (!entries.empty()) {
            event.meta_json = "{";
            for (const auto& [key, value] : entries) {
                if (event.meta_json.size() > 1) {
                    event.meta_json.push_back(',');
                }
                event.meta_json.push_back('"');
                AppendJsonString(event.meta_json, key);
                event.meta_json += "\":";
                event.meta_json += value;
            }
            event.meta_json.push_back('}');
        }
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "emit: field 'meta' must be a table");
    }
    lua_pop(L, 1);

    GetEngine(L).Emit(std::move(event));
    return 0;
}

/// Reads {x, y, width, height} from the table at `index`.
bool ReadRect(lua_State* L, int index, ScreenRect& out) {
    index = lua_absindex(L, index);
    float values[4]{};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, index, i + 1);
        const bool number = lua_type(L, -1) == LUA_TNUMBER;
        values[i] = number ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
        lua_pop(L, 1);
        if (!number) {
            return false;
        }
    }
    out = ScreenRect{values[0], values[1], values[2], values[3]};
    return true;
}

/// More glyphs than this in one text box is a mistake in the profile.
constexpr lua_Integer MaxLayoutGlyphs = 4096;

int ReaderLayout(lua_State* L) {
    const int key_type = lua_type(L, 1);
    luaL_argcheck(L, key_type == LUA_TSTRING || key_type == LUA_TNUMBER, 1,
                  "window key (string or number) expected");
    lua_pushvalue(L, 1); // convert a copy, not the argument
    std::size_t size = 0;
    const char* key = lua_tolstring(L, -1, &size);
    std::string window(key, size);
    lua_pop(L, 1);

    Engine& engine = GetEngine(L);
    if (lua_isnoneornil(L, 2)) {
        engine.CloseLayout(window);
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    TextLayout layout;
    layout.window = std::move(window);

    lua_getfield(L, 2, "space");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        if (lua_type(L, -2) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER) {
            return luaL_error(L, "layout: space must be {width, height}");
        }
        layout.space_width = static_cast<float>(lua_tonumber(L, -2));
        layout.space_height = static_cast<float>(lua_tonumber(L, -1));
        // A picture has a size: anything else is a mistake to be told about, not to be dropped.
        if (!(layout.space_width >= 1.0f && layout.space_width <= 16384.0f) ||
            !(layout.space_height >= 1.0f && layout.space_height <= 16384.0f)) {
            return luaL_error(L, "layout: space must be {width, height}");
        }
        lua_pop(L, 2);
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "layout: space must be {width, height}");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "box");
    if (lua_istable(L, -1)) {
        ScreenRect box;
        if (!ReadRect(L, -1, box)) {
            return luaL_error(L, "layout: box must be {x, y, width, height}");
        }
        layout.box = box;
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "layout: box must be {x, y, width, height}");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "glyphs");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "layout: glyphs must be a list with an entry per character");
    }
    const lua_Integer count = luaL_len(L, -1);
    if (count > MaxLayoutGlyphs) {
        return luaL_error(L, "layout: too many glyphs");
    }
    layout.glyphs.reserve(static_cast<std::size_t>(count));
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, -1, i);
        ScreenRect rect;
        if (lua_istable(L, -1) && ReadRect(L, -1, rect)) {
            layout.glyphs.emplace_back(rect);
        } else {
            layout.glyphs.emplace_back(std::nullopt);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_pushboolean(L, engine.ShowLayout(std::move(layout)));
    return 1;
}

int LogWith(lua_State* L, LogLevel level) {
    std::string message;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        std::size_t size = 0;
        const char* part = luaL_tolstring(L, i, &size);
        if (i > 1) {
            message.push_back(' ');
        }
        message.append(part, size);
        lua_pop(L, 1);
    }
    GetEngine(L).Log(level, SanitizeUtf8(message));
    return 0;
}

int ReaderLog(lua_State* L) {
    return LogWith(L, LogLevel::Info);
}

int ReaderWarn(lua_State* L) {
    return LogWith(L, LogLevel::Warning);
}

int ReaderFrame(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(GetEngine(L).Frame()));
    return 1;
}

/// reader.picture() -> width, height of the picture the game hands over for display, or nothing
/// before it has shown one.
int ReaderPicture(lua_State* L) {
    const auto [width, height] = GetEngine(L).Picture();
    if (width == 0 || height == 0) {
        return 0;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(width));
    lua_pushinteger(L, static_cast<lua_Integer>(height));
    return 2;
}

int ReaderData(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const auto data = GetEngine(L).LoadData(name);
    if (data) {
        PushString(L, *data);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ---- mem ----

template <std::size_t Size>
int MemUnsigned(lua_State* L) {
    const u64 address = CheckAddress(L, 1);
    const std::string bytes = GetEngine(L).ReadMemory(address, Size);
    if (bytes.size() != Size) {
        lua_pushnil(L);
        return 1;
    }
    u64 value = 0;
    for (std::size_t i = 0; i < Size; ++i) {
        value |= static_cast<u64>(static_cast<u8>(bytes[i])) << (8 * i);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

int MemS32(lua_State* L) {
    const u64 address = CheckAddress(L, 1);
    const auto value = GetEngine(L).ReadU32(address);
    if (value) {
        lua_pushinteger(L, static_cast<s32>(*value));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

template <typename Float>
int MemFloat(lua_State* L) {
    const u64 address = CheckAddress(L, 1);
    const std::string bytes = GetEngine(L).ReadMemory(address, sizeof(Float));
    if (bytes.size() != sizeof(Float)) {
        lua_pushnil(L);
        return 1;
    }
    Float value;
    std::memcpy(&value, bytes.data(), sizeof(Float));
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

int MemRead(lua_State* L) {
    const u64 address = CheckAddress(L, 1);
    const lua_Integer size = luaL_checkinteger(L, 2);
    luaL_argcheck(L, size >= 0 && size <= MaxStringRead, 2, "size out of range");
    PushString(L, GetEngine(L).ReadMemory(address, static_cast<std::size_t>(size)));
    return 1;
}

/// Reads units of `unit` bytes until an all-zero unit, the limit or unmapped memory.
int ReadTerminated(lua_State* L, std::size_t unit) {
    const u64 address = CheckAddress(L, 1);
    const lua_Integer limit = luaL_optinteger(L, 2, 4096);
    luaL_argcheck(L, limit >= 0 && limit <= MaxStringRead, 2, "limit out of range");
    const Engine& engine = GetEngine(L);

    std::string out;
    const std::size_t max_bytes = static_cast<std::size_t>(limit);
    bool terminated = false;
    while (!terminated && out.size() < max_bytes) {
        const u64 cursor = address + out.size();
        if (cursor < address) {
            break;
        }
        // Chunks keep unit alignment relative to the start address.
        const std::size_t want = std::min<std::size_t>(256, max_bytes - out.size());
        const std::string chunk = engine.ReadMemory(cursor, want);
        std::size_t used = 0;
        for (; used + unit <= chunk.size(); used += unit) {
            bool zero = true;
            for (std::size_t k = 0; k < unit; ++k) {
                zero = zero && chunk[used + k] == '\0';
            }
            if (zero) {
                terminated = true;
                break;
            }
        }
        out.append(chunk, 0, used);
        if (chunk.size() < want || used == 0) {
            break;
        }
    }
    PushString(L, out);
    return 1;
}

int MemCstr(lua_State* L) {
    return ReadTerminated(L, 1);
}

int MemWstr(lua_State* L) {
    return ReadTerminated(L, 2);
}

// ---- text ----

int TextSjis(lua_State* L) {
    PushString(L, SjisToUtf8(CheckBytes(L, 1)));
    return 1;
}

int TextUtf16le(lua_State* L) {
    PushString(L, Utf16ToUtf8(CheckBytes(L, 1), false));
    return 1;
}

int TextUtf16be(lua_State* L) {
    PushString(L, Utf16ToUtf8(CheckBytes(L, 1), true));
    return 1;
}

int TextHash(lua_State* L) {
    PushString(L, HashHex(CheckBytes(L, 1)));
    return 1;
}

int TextLen(lua_State* L) {
    std::size_t size = 0;
    const char* data = luaL_checklstring(L, 1, &size);
    lua_pushinteger(L, static_cast<lua_Integer>(CountCodePoints(std::string_view(data, size))));
    return 1;
}

const luaL_Reg ReaderFunctions[] = {
    {"hook", ReaderHook},   {"on_frame", ReaderOnFrame}, {"module", ReaderModule},
    {"emit", ReaderEmit},   {"log", ReaderLog},          {"warn", ReaderWarn},
    {"frame", ReaderFrame}, {"data", ReaderData},        {"layout", ReaderLayout},
    {"picture", ReaderPicture}, {"on_state_loaded", ReaderOnStateLoaded},
    {nullptr, nullptr},
};

const luaL_Reg MemFunctions[] = {
    {"u8", MemUnsigned<1>}, {"u16", MemUnsigned<2>}, {"u32", MemUnsigned<4>},
    {"u64", MemUnsigned<8>}, {"s32", MemS32},        {"f32", MemFloat<float>},
    {"f64", MemFloat<double>}, {"read", MemRead},    {"cstr", MemCstr},
    {"wstr", MemWstr},      {nullptr, nullptr},
};

const luaL_Reg TextFunctions[] = {
    {"sjis", TextSjis}, {"utf16le", TextUtf16le}, {"utf16be", TextUtf16be},
    {"hash", TextHash}, {"len", TextLen},         {nullptr, nullptr},
};

/// Helpers that are simpler to express in Lua than through the C API.
const char Prelude[] = R"lua(
local budget_exhausted = ...

-- A callback that ran out of budget ends, even when the error passes through a pcall of the
-- profile's own.
do
    local raw_pcall, raw_xpcall, raw_resume, error = pcall, xpcall, coroutine.resume, error
    local function pass(ok, ...)
        if not ok and budget_exhausted() then
            error((...), 0)
        end
        return ok, ...
    end
    function pcall(f, ...)
        return pass(raw_pcall(f, ...))
    end
    function xpcall(f, handler, ...)
        return pass(raw_xpcall(f, handler, ...))
    end
    function coroutine.resume(co, ...)
        return pass(raw_resume(co, ...))
    end
end

local Builder = {}
Builder.__index = Builder

-- Accumulates display text and ruby spans; offsets are counted in code points.
function reader.builder()
    return setmetatable({ parts = {}, count = 0, spans = {} }, Builder)
end

function Builder:add(s)
    if s ~= nil and s ~= "" then
        self.parts[#self.parts + 1] = s
        self.count = self.count + text.len(s)
    end
    return self
end

function Builder:add_ruby(base, reading)
    local length = text.len(base)
    if length > 0 and reading ~= nil and reading ~= "" then
        self.spans[#self.spans + 1] = { self.count, length, reading }
    end
    return self:add(base)
end

function Builder:text()
    return table.concat(self.parts)
end

function Builder:ruby()
    return self.spans
end

function Builder:length()
    return self.count
end

-- Compiled chunks are not checked by Lua and could corrupt memory: source text only.
do
    local raw_load, select = load, select
    function load(chunk, name, mode, ...)
        if select("#", ...) == 0 then
            return raw_load(chunk, name, "t")
        end
        return raw_load(chunk, name, "t", (...))
    end
end

-- There is no console; print goes to the log.
function print(...)
    reader.log(...)
end

local last_values = {}

-- True when `value` differs from the value last passed for `slot`.
function reader.changed(slot, value)
    if last_values[slot] == value then
        return false
    end
    last_values[slot] = value
    return true
end

function reader.forget(slot)
    if slot == nil then
        last_values = {}
    else
        last_values[slot] = nil
    end
end
)lua";

/// lua_CFunction: sets up the state at index 1 (the engine, as light userdata).
int Install(lua_State* L) {
    Engine& engine = *static_cast<Engine*>(lua_touserdata(L, 1));

    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const auto& library : libraries) {
        luaL_requiref(L, library.name, library.func, 1);
        lua_pop(L, 1);
    }
    // Profiles get no file or process access.
    for (const char* name : {"dofile", "loadfile"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    lua_pushlightuserdata(L, &engine);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &EngineKey);

    luaL_newlib(L, ReaderFunctions);
    const std::string title = engine.GameId();
    lua_pushlstring(L, title.data(), title.size());
    lua_setfield(L, -2, "game_id");
    lua_pushlstring(L, title.data(), title.size());
    lua_setfield(L, -2, "title_id"); // the name it has in the reader's Switch and 3DS siblings
    // The game's own executable: where it is, and which build it is.
    if (const auto main = engine.FindModule("main")) {
        lua_pushinteger(L, static_cast<lua_Integer>(main->base));
        lua_setfield(L, -2, "base");
        lua_pushlstring(L, main->build_id.data(), main->build_id.size());
        lua_setfield(L, -2, "build_id");
    }
    lua_setglobal(L, "reader");

    luaL_newlib(L, MemFunctions);
    lua_setglobal(L, "mem");

    luaL_newlib(L, TextFunctions);
    lua_setglobal(L, "text");

    auto* view = static_cast<CpuView*>(lua_newuserdatauv(L, sizeof(CpuView), 0));
    *view = CpuView{};
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, CpuIndex);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &CpuKey);

    lua_sethook(L, BudgetHook, LUA_MASKCOUNT, BudgetStep);

    if (luaL_loadbufferx(L, Prelude, sizeof(Prelude) - 1, "=reader prelude", "t") != LUA_OK) {
        return lua_error(L);
    }
    lua_pushcfunction(L, BudgetExhausted);
    lua_call(L, 1, 0);
    return 0;
}

} // Anonymous namespace

lua_State* NewState(Engine& engine) {
    auto data = std::make_unique<StateData>();
    lua_State* L = lua_newstate(LimitedAlloc, data.get());
    if (!L) {
        return nullptr;
    }
    data->budget = LoadBudgetSteps;
    data.release(); // owned by the state from here on, see CloseState

    lua_pushcfunction(L, Install);
    lua_pushlightuserdata(L, &engine);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        engine.Log(LogLevel::Error,
                   std::format("Lua state setup failed: {}", message ? message : "unknown error"));
        CloseState(L);
        return nullptr;
    }
    return L;
}

void CloseState(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    lua_close(L);
    delete static_cast<StateData*>(ud);
}

void ResetBudget(lua_State* L, Budget kind) {
    BudgetLeft(L) = kind == Budget::Load ? LoadBudgetSteps : CallbackBudgetSteps;
}

int CallHook(lua_State* L) {
    const auto* regs = static_cast<const GuestRegisters*>(lua_touserdata(L, 2));
    lua_settop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &CpuKey);
    auto* view = static_cast<CpuView*>(lua_touserdata(L, -1));
    view->regs = *regs;
    view->live = true;
    // Lua errors are C++ exceptions in this build, so this also runs when the handler fails.
    struct Expire {
        CpuView* view;
        ~Expire() {
            view->live = false;
        }
    } expire{view};
    lua_call(L, 1, 0);
    return 0;
}

int CallAndFormat(lua_State* L) {
    lua_call(L, 0, LUA_MULTRET);
    const int count = lua_gettop(L);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 1; i <= count; ++i) {
        if (i > 1) {
            luaL_addchar(&buffer, '\t');
        }
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    return 1;
}

int Traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (!message) {
        message = luaL_tolstring(L, 1, nullptr);
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

} // namespace Reader::LuaApi
