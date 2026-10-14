// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// All that the emulator's Qt frontend sees of the reader.

#include <QPointF>

class QMainWindow;
class QOpenGLWidget;

namespace ReaderUi {

/// Adds the reader to the emulator's window: its menu, its layer over the game's picture.
void Install(QMainWindow* main_window, QOpenGLWidget* render_widget);

// The pointer (the mouse, or a finger) on the widget the game is drawn in, before the game
// gets it. True when the reader keeps the event.
bool PointerPress(const QPointF& position, bool left_button);
bool PointerMove(const QPointF& position);
bool PointerRelease(const QPointF& position);

/// The render widget drew a frame (its OpenGL context is current): the development channel
/// may have asked for a picture of it.
void SurfaceDrawn(unsigned int framebuffer, int width, int height);

} // namespace ReaderUi
