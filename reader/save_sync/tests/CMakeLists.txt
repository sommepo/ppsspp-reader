# SPDX-FileCopyrightText: PPSSPP Reader fork contributors
# SPDX-License-Identifier: GPL-2.0-or-later

# Reader fork: tests of save sync with made-up saves. Those of the engine and of the PSP's
# side (psp.cpp) talk to a real service, which tools/reader/test-savesync.sh starts on a
# loopback port with a folder of its own; without one they are skipped.
add_executable(reader_save_sync_tests
    archive.cpp
    decide.cpp
    engine.cpp
    psp.cpp
    test_util.h
)
target_include_directories(reader_save_sync_tests SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/reader-json)
target_link_libraries(reader_save_sync_tests PRIVATE reader_save_sync Catch2::Catch2WithMain)
target_compile_options(reader_save_sync_tests PRIVATE -fexceptions)
set_target_properties(reader_save_sync_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
