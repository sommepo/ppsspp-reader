// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include "reader/core/text_codec.h"

namespace Reader {

namespace {

constexpr std::size_t SjisTrailCount = 189;
constexpr std::array<u16, 60 * SjisTrailCount> SjisTable{{
#include "reader/core/sjis_table.inc"
}};

constexpr u32 ReplacementChar = 0xFFFD;

/// Returns the lead-byte row in SjisTable, or -1 when the byte is not a lead byte.
constexpr int SjisLeadRow(u8 lead) {
    if (lead >= 0x81 && lead <= 0x9F) {
        return lead - 0x81;
    }
    if (lead >= 0xE0 && lead <= 0xFC) {
        return 31 + (lead - 0xE0);
    }
    return -1;
}

} // Anonymous namespace

void AppendUtf8(std::string& out, u32 cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = ReplacementChar;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string SjisToUtf8(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size() * 3 / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const u8 b = bytes[i];
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            continue;
        }
        if (b >= 0xA1 && b <= 0xDF) {
            // Half-width katakana block
            AppendUtf8(out, 0xFF61 + (b - 0xA1));
            continue;
        }
        const int row = SjisLeadRow(b);
        if (row < 0 || i + 1 >= bytes.size()) {
            AppendUtf8(out, ReplacementChar);
            continue;
        }
        const u8 trail = bytes[i + 1];
        if (trail < 0x40 || trail > 0xFC) {
            // Not a trail byte: report the lead alone and reconsider this byte next.
            AppendUtf8(out, ReplacementChar);
            continue;
        }
        const u16 cp = SjisTable[static_cast<std::size_t>(row) * SjisTrailCount + (trail - 0x40)];
        if (cp != 0) {
            AppendUtf8(out, cp);
            ++i;
            continue;
        }
        // Unassigned. An ASCII byte in the trail position is read again on its own, so that a
        // stray lead byte cannot swallow the character after it (as browsers decode it).
        AppendUtf8(out, ReplacementChar);
        if (trail >= 0x80) {
            ++i;
        }
    }
    return out;
}

std::string Utf16ToUtf8(std::span<const u8> bytes, bool big_endian) {
    std::string out;
    out.reserve(bytes.size() * 3 / 2);
    const auto unit = [&](std::size_t i) -> u32 {
        return big_endian ? (bytes[i] << 8) | bytes[i + 1] : bytes[i] | (bytes[i + 1] << 8);
    };
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        u32 cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 3 < bytes.size()) {
                const u32 low = unit(i + 2);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else {
                    cp = ReplacementChar;
                }
            } else {
                cp = ReplacementChar;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = ReplacementChar;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::string SanitizeUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const u8 b0 = static_cast<u8>(bytes[i]);
        std::size_t len = 0;
        u32 cp = 0;
        u32 min = 0;
        if (b0 < 0x80) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            len = 2, cp = b0 & 0x1F, min = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3, cp = b0 & 0x0F, min = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4, cp = b0 & 0x07, min = 0x10000;
        }
        bool ok = len != 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) {
            const u8 b = static_cast<u8>(bytes[i + k]);
            if ((b & 0xC0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (b & 0x3F);
            }
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            ok = false;
        }
        if (ok) {
            out.append(bytes.substr(i, len));
            i += len;
        } else {
            AppendUtf8(out, ReplacementChar);
            ++i;
        }
    }
    return out;
}

std::size_t CountCodePoints(std::string_view utf8) {
    std::size_t count = 0;
    for (const char c : utf8) {
        // Count every byte that is not a continuation byte.
        if ((static_cast<u8>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string HashHex(std::span<const u8> bytes) {
    u64 hash = 0xCBF29CE484222325ULL;
    for (const u8 b : bytes) {
        hash ^= b;
        hash *= 0x100000001B3ULL;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = digits[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

void AppendJsonString(std::string& out, std::string_view utf8) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const char ch : utf8) {
        const u8 c = static_cast<u8>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(digits[c >> 4]);
                out.push_back(digits[c & 0xF]);
            } else {
                out.push_back(ch);
            }
        }
    }
}

} // namespace Reader
