// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <QImage>
#include <QRectF>
#include <QSize>

namespace ReaderUi {

/**
 * The tube's frame around the game's picture, as a photograph of an old monitor would show it:
 * the moulded plastic of the case (charcoal, black or beige, the CRT settings' "Frame"), the
 * deep recess the glass sits in, lit by a lamp in the room or only by the picture, the dark
 * rubber seal along the glass, the shadow the recess casts onto the glass, the picture's light
 * on the recess, and dust, grime and age as much as "Wear" says.
 *
 * After Morgawr's michadame (its fs_retro_frame shader), drawn here once for a window's size
 * rather than for every frame: the frame stands still, the picture moves behind it. The opening
 * follows the tube's edge as the CRT shader draws it (warp and corners), a little inside it.
 *
 * Drawn by the reader's layer over the game, under everything else of it, with the CRT picture
 * on and a frame chosen. The same in every fork (tools/reader/crt.py sync).
 */
class CrtFrame {
public:
    CrtFrame();
    ~CrtFrame();

    /// The window is `size` (its own pixels) at `device_pixel_ratio`, the game's picture at
    /// `picture` in it. A new frame is made in the background when anything of it changed (the
    /// CRT settings too); `ready` is then called, on this thread, to have it drawn.
    void Update(QSize size, QRectF picture, qreal device_pixel_ratio);

    /// The frame to draw over the whole window: what Update last made, or null (none chosen,
    /// the CRT picture off, or not made yet).
    [[nodiscard]] const QImage& Image() const {
        return image;
    }
    /// Whether a frame is wanted at all just now: the window is to be shown for it.
    [[nodiscard]] bool Wanted() const;

    std::function<void()> ready;

    /// The frame for a window of `size` device pixels around `picture` (device pixels), as
    /// the CRT settings say now. Null when none is chosen. For tests and pictures of it.
    [[nodiscard]] static QImage Draw(QSize size, QRectF picture);

private:
    QImage image;
    QSize made_size;
    QRectF made_picture;
    unsigned made_revision = ~0u;
    std::shared_ptr<std::atomic<unsigned>> generation;
};

} // namespace ReaderUi
