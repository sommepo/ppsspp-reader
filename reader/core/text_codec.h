// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>
#include "reader/core/types.h"

namespace Reader {

/// Appends one Unicode scalar value to a UTF-8 string. Invalid values become U+FFFD.
void AppendUtf8(std::string& out, u32 code_point);

/// Decodes CP932 (Shift-JIS) bytes. Unassigned or truncated sequences become U+FFFD.
std::string SjisToUtf8(std::span<const u8> bytes);

/// Decodes UTF-16 bytes, stopping at the end of the buffer. Lone surrogates become U+FFFD.
std::string Utf16ToUtf8(std::span<const u8> bytes, bool big_endian);

/// Returns the input with every malformed UTF-8 sequence replaced by U+FFFD.
std::string SanitizeUtf8(std::string_view bytes);

/// Number of Unicode scalar values in a UTF-8 string (malformed bytes count as one each).
std::size_t CountCodePoints(std::string_view utf8);

/// 64-bit FNV-1a, formatted as 16 lowercase hex digits. Used for stable text source IDs.
std::string HashHex(std::span<const u8> bytes);

/// Escapes a UTF-8 string for inclusion inside JSON double quotes.
void AppendJsonString(std::string& out, std::string_view utf8);

} // namespace Reader
