# The reader fork's own code. The emulator's build includes this file once, near its end.
#
#   reader_lua   Lua, built as C++ (its errors are then exceptions, which the bindings rely
#                on to unwind). The emulator has a Lua of its own, built as C, for its own use;
#                the two do not meet.
#   reader_core  The text engine: profiles, hooks, text events. Knows nothing of the emulator.
#   reader/core/emulator.cpp is the one file that ties it to the emulator, and is built as
#   part of the emulator's core.

option(READER_HARNESS "Build the reader's headless harness (Linux)" OFF)
option(READER_TESTS "Build the reader's C++ tests" OFF)

set(READER_LUA_DIR ${CMAKE_SOURCE_DIR}/ext/lua)
set(READER_LUA_SOURCES)
foreach(name lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex
		lmathlib lmem lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm
		lundump lutf8lib lvm lzio)
	list(APPEND READER_LUA_SOURCES ${READER_LUA_DIR}/${name}.c)
endforeach()
set_source_files_properties(${READER_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(reader_lua STATIC ${READER_LUA_SOURCES})
target_include_directories(reader_lua PUBLIC ${READER_LUA_DIR})
# Functions get C++ names of their own; the few data symbols would collide with the
# emulator's Lua and are renamed.
target_compile_definitions(reader_lua PRIVATE
	luai_ctype_=reader_luai_ctype_ luaT_typenames_=reader_luaT_typenames_
	luaP_opmodes=reader_luaP_opmodes lua_ident=reader_lua_ident)
target_compile_options(reader_lua PRIVATE -fexceptions -w)
set_target_properties(reader_lua PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)

add_library(reader_core STATIC
	core/audio_tap.cpp
	core/audio_tap.h
	core/crt.cpp
	core/crt.h
	core/engine.cpp
	core/engine.h
	core/input_filter.cpp
	core/input_filter.h
	core/lua_api.cpp
	core/lua_api.h
	core/text_codec.cpp
	core/text_codec.h
	core/types.h
)
target_include_directories(reader_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(reader_core PUBLIC reader_lua)
# The CRT picture is on in an install that was never told otherwise (reader/core/crt.cpp):
# this console was played on a tube.
target_compile_definitions(reader_core PRIVATE READER_CRT_DEFAULT_ON)
target_compile_options(reader_core PRIVATE -fexceptions)
set_target_properties(reader_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
	POSITION_INDEPENDENT_CODE ON)

# Save sync: the Eden Reader fork's engine (archive, remote, engine) and what fits it to the
# PSP's save folders (host). Knows nothing of the emulator either. cpp-httplib and nlohmann's
# JSON are single headers kept in ext/ (plain HTTP: the service is in the home network).
add_library(reader_save_sync STATIC
	save_sync/archive.cpp
	save_sync/archive.h
	save_sync/engine.cpp
	save_sync/engine.h
	save_sync/host.cpp
	save_sync/host.h
	save_sync/remote.cpp
	save_sync/remote.h
)
target_include_directories(reader_save_sync PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(reader_save_sync SYSTEM PRIVATE
	${CMAKE_SOURCE_DIR}/ext/reader-httplib ${CMAKE_SOURCE_DIR}/ext/reader-json)
target_link_libraries(reader_save_sync PRIVATE Common ${ZLIB_LIBRARY})
if(WIN32)
	target_link_libraries(reader_save_sync PRIVATE ws2_32)
endif()
target_compile_options(reader_save_sync PRIVATE -fexceptions)
set_target_properties(reader_save_sync PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
	POSITION_INDEPENDENT_CODE ON)

# The emulator's side.
target_sources(${CoreLibName} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/core/emulator.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/core/emulator.h)
target_link_libraries(${CoreLibName} reader_core reader_save_sync)

if(READER_HARNESS)
	add_executable(ppsspp-reader-harness harness/harness.cpp)
	target_link_libraries(ppsspp-reader-harness ${LinkCommon} Common reader_core)
	target_compile_options(ppsspp-reader-harness PRIVATE -fexceptions)
	set_target_properties(ppsspp-reader-harness PROPERTIES CXX_STANDARD 20)
endif()

# The desktop app's reading logic, free of the emulator (dictionaries, lookup, cards,
# statistics): the Android layer's Kotlin in C++. Built with the Qt frontend.
if(USING_QT_UI)
	add_subdirectory(desktop)

	# The reader's windows in the emulator's Qt frontend, and the bundled profiles as
	# resources of the program.
	set(CMAKE_AUTOMOC ON)
	find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network OpenGLWidgets)
	add_library(reader_qt STATIC
		qt/audio_prompt.cpp qt/audio_prompt.h
		qt/host.cpp qt/host.h qt/install.h
		qt/lookup_flow.cpp qt/lookup_flow.h
		qt/lookup_popup.cpp qt/lookup_popup.h
		qt/reader_control.cpp qt/reader_control.h
		qt/reader_controller.cpp qt/reader_controller.h
		qt/reader_crt_page.cpp qt/reader_crt_page.h
		qt/reader_cursor.cpp qt/reader_cursor.h
		qt/reader_dialogs.cpp qt/reader_dialogs.h
		qt/reader_games.cpp qt/reader_games.h
		qt/reader_media.cpp qt/reader_media.h
		qt/reader_overlay.cpp qt/reader_overlay.h qt/reader_crt_frame.cpp qt/reader_crt_frame.h
		qt/reader_pad.cpp qt/reader_pad.h
		qt/reader_link.cpp qt/reader_link.h
		qt/reader_steam.cpp qt/reader_steam.h
		qt/reader_session.cpp qt/reader_session.h
	)
	# A published build (READER_BUNDLE_PROFILES=OFF) comes with no game scripts: they are
	# installed from the published list (Reader, Game scripts).
	option(READER_BUNDLE_PROFILES "Build the game scripts in assets/reader/profiles into the app" ON)
	if(READER_BUNDLE_PROFILES)
		file(GLOB READER_PROFILE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/reader/profiles/*")
		qt_add_resources(reader_qt "reader_profiles"
			PREFIX "/reader/profiles"
			BASE "${CMAKE_SOURCE_DIR}/assets/reader/profiles"
			FILES ${READER_PROFILE_FILES})
	endif()
	qt_add_resources(reader_qt "reader_icon"
		PREFIX "/reader/icon"
		BASE "${CMAKE_BINARY_DIR}/reader_icon"
		FILES "${CMAKE_BINARY_DIR}/reader_icon/app.png")
	target_include_directories(reader_qt PUBLIC ${CMAKE_SOURCE_DIR})
	target_link_libraries(reader_qt PUBLIC reader_desktop reader_core Qt6::Widgets Qt6::Gui Qt6::Core
		Qt6::Network Qt6::OpenGLWidgets)
	if(SDL2_FOUND)
		target_link_libraries(reader_qt PRIVATE SDL2::SDL2)
	endif()
	# The emulator's input library is SDL 2: the Steam Controller's gamepad is made through it.
	target_compile_definitions(reader_qt PRIVATE READER_SDL_MAJOR=2)
	if(WIN32)
		# The Steam Controller's back buttons are read from the device itself.
		target_link_libraries(reader_qt PRIVATE hid setupapi)
	endif()
	target_compile_options(reader_qt PRIVATE -fexceptions)
	set_target_properties(reader_qt PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_link_libraries(${TargetBin} reader_qt)
	if(READER_TESTS)
		# After reader/desktop, whose tests fetch Catch2.
		add_subdirectory(save_sync/tests)
	endif()
endif()
