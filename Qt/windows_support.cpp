// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Reader fork: the Qt frontend on Windows. The emulator's shared code asks a few things of
// its own Windows frontend when it is built for Windows; this file answers them for the
// Qt frontend, which is the app there.

#ifdef _WIN32

#include <QMainWindow>

#include "Common/CommonWindows.h"

extern QMainWindow *g_mainWindowForWin32;

namespace MainWindow {
// The top window, for the system's own dialogs (the folder chooser).
HWND GetHWND() {
	return g_mainWindowForWin32 ? reinterpret_cast<HWND>(g_mainWindowForWin32->winId()) : nullptr;
}
}  // namespace MainWindow

// Qt's entry code for Windows calls the program's main by the name qMain, and SDL's header
// renames the real one (Qt/QtMain.cpp) to SDL_main. "main" cannot be named here: in this
// file it would be qMain itself, a function that calls itself for ever, which is what the
// first Windows package did at its start (no window, one core busy).
#ifdef SDL
extern "C" int SDL_main(int argc, char *argv[]);
int qMain(int argc, char **argv) {
	return SDL_main(argc, argv);
}
#endif

#endif
