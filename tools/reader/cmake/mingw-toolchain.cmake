# SPDX-FileCopyrightText: Eden Reader fork contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Reader fork: building the desktop app for Windows on Linux, with the MinGW compiler of
# tools/reader/env.sh (READER_MINGW) and the official Qt for MinGW (READER_QT_WINDOWS). Used by
# tools/reader/build-windows.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_mingw "$ENV{READER_MINGW}")
set(CMAKE_C_COMPILER "${_mingw}/bin/x86_64-w64-mingw32-gcc-posix")
set(CMAKE_CXX_COMPILER "${_mingw}/bin/x86_64-w64-mingw32-g++-posix")
set(CMAKE_RC_COMPILER "${_mingw}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${_mingw}/bin/x86_64-w64-mingw32-gcc-ar-posix")
set(CMAKE_RANLIB "${_mingw}/bin/x86_64-w64-mingw32-gcc-ranlib-posix")

# Libraries and headers come from the Windows side only; programs (moc, the shader compiler)
# from this machine.
set(CMAKE_FIND_ROOT_PATH "${_mingw}/x86_64-w64-mingw32" "$ENV{READER_QT_WINDOWS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
