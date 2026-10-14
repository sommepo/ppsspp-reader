# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later

# Reader fork: tests of the desktop reader layer's logic, with made-up text and dictionaries
# written for the test (tools/reader/test-desktop.sh). They are the Android app's JVM tests of
# the same parts, as far as those parts are the same.
add_executable(reader_desktop_tests
    anki.cpp
    card_builder.cpp
    word_audio.cpp
    catalog.cpp
    # The CRT picture's values and what the shader is handed (crt.h in the reader's core),
    # which has nothing of the emulator in it and is built into the tests as it is.
    crt_test.cpp
    ${CMAKE_SOURCE_DIR}/reader/core/crt.cpp
    deinflector.cpp
    dictionary.cpp
    discord_presence.cpp
    glossary.cpp
    media.cpp
    screen_text.cpp
    script_index.cpp
    stats.cpp
    steam_bridge.cpp
    test_util.h
    text.cpp
    text_hook_server.cpp
)

FetchContent_Declare(reader_catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz)
FetchContent_MakeAvailable(reader_catch2)
find_package(Threads REQUIRED)
set_target_properties(reader_desktop_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(reader_desktop_tests PRIVATE -fexceptions)
set(READER_BUNDLED_PROFILE_DIR "${CMAKE_SOURCE_DIR}/assets/reader/profiles")
target_compile_definitions(reader_desktop_tests PRIVATE
    READER_BUNDLED_PROFILE_DIR="${READER_BUNDLED_PROFILE_DIR}")
target_link_libraries(reader_desktop_tests PRIVATE reader_desktop QuaZip::QuaZip
                                                   Catch2::Catch2WithMain Threads::Threads)
if (NOT MSVC)
    target_compile_options(reader_desktop_tests PRIVATE
        -Wno-shadow -Wno-conversion -Wno-unused-variable -Wno-unused-parameter
        -Wno-missing-field-initializers)
endif()
