# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later

# Reader fork: what the desktop app's reader layer is made of apart from its windows:
# dictionaries, lookups, cards, statistics, the backlog. It is the Android app's reader layer
# (src/android/.../reader) in C++, kept free of the emulator so that it can be tested alone
# (tools/reader/test-desktop.sh). The windows are in src/yuzu/reader.
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

# What the desktop reading logic is made with besides Qt, fetched at configure time: SQLite
# (dictionaries, backlog, statistics), QuaZip (dictionary files are zip archives) and Opus
# (the audio of a card).
include(FetchContent)
FetchContent_Declare(reader_sqlite_src
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    URL_HASH SHA512=71e9d81ee7e1160b9da6da0d390e3bd7dc8b434b157db6ba0e0a95446ae50c95488af23b93ecb02ea8b5c305cb16d9e20b174829e41dc17ec747efcb6a7e64d1)
FetchContent_MakeAvailable(reader_sqlite_src)
add_library(reader_sqlite STATIC EXCLUDE_FROM_ALL ${reader_sqlite_src_SOURCE_DIR}/sqlite3.c)
target_include_directories(reader_sqlite SYSTEM PUBLIC ${reader_sqlite_src_SOURCE_DIR})
target_compile_definitions(reader_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
target_compile_options(reader_sqlite PRIVATE -w)

set(QUAZIP_QT_MAJOR_VERSION 6 CACHE STRING "" FORCE)
set(QUAZIP_INSTALL OFF CACHE BOOL "" FORCE)
set(QUAZIP_ENABLE_QTEXTCODEC OFF CACHE BOOL "" FORCE)
set(QUAZIP_BZIP2 OFF CACHE BOOL "" FORCE)
set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)
# Where the emulator builds its own zlib (Windows), QuaZip is pointed at that one.
if(TARGET zlib)
    # For this directory only: the emulator's own search for a zlib must go on failing.
    set(ZLIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/ext/zlib")
    if(CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
        set(ZLIB_LIBRARY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libzlib.a")
    else()
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/libzlib.a")
    endif()
endif()
set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(reader_quazip
    URL https://github.com/stachenov/quazip/archive/2e95c9001b.tar.gz)
FetchContent_Declare(reader_opus
    URL https://github.com/xiph/opus/archive/a3f0ec02b3.tar.gz)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(reader_quazip reader_opus)
set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
if(TARGET zlib)
    add_dependencies(QuaZip zlib)
endif()
if(NOT TARGET Opus::opus)
    add_library(Opus::opus ALIAS opus)
endif()

add_library(reader_desktop STATIC
    anki.cpp
    anki.h
    card_builder.cpp
    card_builder.h
    catalog.cpp
    catalog.h
    deinflector.cpp
    deinflector.h
    dictionary.cpp
    dictionary.h
    discord_presence.cpp
    discord_presence.h
    dictionary_importer.cpp
    glossary.cpp
    glossary.h
    japanese_text.cpp
    japanese_text.h
    media.cpp
    media.h
    reader_database.cpp
    reader_database.h
    screen_text.cpp
    screen_text.h
    script_index.cpp
    script_index.h
    settings.cpp
    settings.h
    sqlite_db.cpp
    sqlite_db.h
    stats.cpp
    steam_bridge.cpp
    steam_bridge.h
    stats.h
    text_hook_server.cpp
    text_hook_server.h
    text_line.cpp
    text_line.h
    word_audio.cpp
    word_audio.h
)

target_link_libraries(reader_desktop PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
target_link_libraries(reader_desktop PRIVATE reader_sqlite QuaZip::QuaZip Opus::opus)
target_include_directories(reader_desktop PUBLIC ${CMAKE_SOURCE_DIR})
if(TARGET zlib)
    target_link_libraries(reader_desktop PRIVATE zlib)
    target_include_directories(reader_desktop PRIVATE ${CMAKE_SOURCE_DIR}/ext/zlib)
endif()
set_target_properties(reader_desktop PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(reader_desktop PRIVATE -fexceptions)
target_compile_definitions(reader_desktop PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
if (NOT MSVC)
    target_compile_options(reader_desktop PRIVATE -Wno-shadow -Wno-conversion)
endif()

if (READER_TESTS)
    add_subdirectory(tests)
endif()
