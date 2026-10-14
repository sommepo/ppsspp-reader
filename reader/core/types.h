// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The reader's own code names its integer types as the emulator does. This header is all it
// needs of that, so that the reading logic also builds without the emulator (tests, tools).
#include <cstddef>
#include <cstdint>

// The same types as the emulator's Common/CommonTypes.h, so that both can be included.
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
