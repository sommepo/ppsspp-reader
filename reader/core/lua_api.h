// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

struct lua_State;

namespace Reader {

class Engine;

namespace LuaApi {

/**
 * Creates a Lua state for a profile: memory-capped, with the sandboxed standard libraries and the
 * `reader`, `mem` and `text` tables. Returns nullptr when out of memory. Release it with
 * CloseState, not lua_close.
 */
lua_State* NewState(Engine& engine);
void CloseState(lua_State* L);

enum class Budget { Callback, Load };

/// Restores the instruction budget before entering profile code.
void ResetBudget(lua_State* L, Budget kind = Budget::Callback);

// The C++ side must not call Lua functions that can allocate outside a protected call: at the
// memory cap they raise an error, and an unprotected error ends the process. These two do such
// work inside the call they are part of.

/// lua_CFunction (handler, GuestRegisters* as light userdata): calls handler(cpu). `cpu` reads
/// the registers on demand and stops working when the handler returns.
int CallHook(lua_State* L);

/// lua_CFunction (chunk): calls it and returns its results as one tab-separated string.
int CallAndFormat(lua_State* L);

/// Message handler for lua_pcall that appends a stack traceback.
int Traceback(lua_State* L);

} // namespace LuaApi

} // namespace Reader
