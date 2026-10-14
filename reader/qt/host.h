// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the desktop reader's windows need of the emulator, in one place. The windows were
// written for another emulator's desktop app (the Eden Reader fork); this header gives them
// the few things they ask of their surroundings in the shape they ask for them.

#include <format>
#include <functional>
#include <string>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QWidget>

namespace ReaderUi {

enum class HostLog { Info, Warning, Error };
/// A line for the emulator's log.
void WriteLog(HostLog level, const std::string& message);

/// The folder the reader keeps its own files in (dictionaries, history, settings, profiles
/// of the user's own): "reader" in the emulator's folder for the PSP's memory stick.
[[nodiscard]] QString ReaderDirectory();

/**
 * Runs something on the main thread soon (within a few milliseconds), in the order asked.
 * **This is how the emulator's own threads reach the reader's Qt side**: the loader, the
 * CPU and the audio threads are not Qt's, and on Windows a thread that has used Qt objects
 * and then ends takes the program down with it (the first Windows package froze at every
 * game start that way). Nothing of Qt is touched by the call itself.
 */
void PostToGui(std::function<void()> task);
/// Where short-lived files go (a card's picture and sound on their way to Anki).
[[nodiscard]] QString CacheDirectory();

/**
 * Who gets the pointer first: where the game shows text of its own, a click is the reader's.
 * Positions are those of the widget the game is drawn in.
 */
class ReaderPointerFilter {
public:
    virtual ~ReaderPointerFilter() = default;
    /// The pointer went down. True when the reader takes it: the game does not hear of it.
    virtual bool OnPointerPress(const QPointF& position, bool left_button) = 0;
    virtual void OnPointerMove(const QPointF& position) = 0;
    /// The pointer of a press the reader took went up.
    virtual void OnPointerRelease(const QPointF& position) = 0;
};

/// Where the pointer is on the screen. The system's answer, unless the development channel
/// has said where it is: on a screen without a pointer device there is nobody to ask, and
/// moving the system's pointer there takes the X server down.
[[nodiscard]] QPoint PointerPosition();
void SetPointerPositionForTests(const QPoint& global);

/// For the development control channel: the emulator's own picture (its screens too, which
/// the capture of a game's frame does not have) is written to `path` after the next frame.
void RequestSurfaceGrab(const QString& path);

/// What the emulator's window does for the reader.
struct EmulatorHost {
    /// A game is running (not paused, not behind the emulator's own menus).
    std::function<bool()> is_running;
    /// A game is loaded, running or paused.
    std::function<bool()> is_powered_on;
    std::function<void()> pause;
    std::function<void()> resume;
    /// Where the game's picture is in the widget it is drawn in, in the widget's own units.
    std::function<QRectF()> picture_rect;
    /// Asks for the game's picture as it is now; the answer (a null image when there is
    /// none) comes on the main thread, a frame or so later.
    std::function<void(std::function<void(const QImage&)>)> grab_picture;
    /// Sets who gets the pointer first, or nobody.
    std::function<void(ReaderPointerFilter*)> set_pointer_filter;
};

} // namespace ReaderUi

// The windows log through these, with std::format's placeholders.
#define LOG_INFO(category, ...) ::ReaderUi::WriteLog(::ReaderUi::HostLog::Info, std::format(__VA_ARGS__))
#define LOG_WARNING(category, ...) ::ReaderUi::WriteLog(::ReaderUi::HostLog::Warning, std::format(__VA_ARGS__))
#define LOG_ERROR(category, ...) ::ReaderUi::WriteLog(::ReaderUi::HostLog::Error, std::format(__VA_ARGS__))
