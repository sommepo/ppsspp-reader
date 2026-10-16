// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QColor>
#include <QCursor>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class QPainter;
#include "reader/desktop/screen_text.h"
#include "reader/qt/reader_crt_frame.h"
#include "reader/qt/reader_mark_glide.h"

namespace ReaderUi {

/// Colours of the reader's in-game surfaces. They sit over game pictures, so they are fixed
/// rather than following the app's theme. Those of the Android app.
namespace Style {
inline const QColor PopupBackground{0x1A, 0x1F, 0x27};
inline const QColor Outline{0xFF, 0xFF, 0xFF, 0x55};
inline const QColor Text{0xF4, 0xF4, 0xF2};
inline const QColor TextDim{0xAE, 0xB4, 0xBC};
inline const QColor TextReread{0xFF, 0x9E, 0x8A};
inline const QColor Accent{0xFF, 0xD5, 0x4F};
inline const QColor Highlight{0xFF, 0xD5, 0x4F, 0x66};
inline const QColor Box{0x2A, 0x31, 0x3B};
inline const QColor BoxPressed{0x55, 0x60, 0x6E};
inline const QColor CursorEdge{0x00, 0x00, 0x00, 0xD0};
inline const QColor SessionText{0xFF, 0xFF, 0xFF, 0xB8};
inline const QColor SessionShadow{0x00, 0x00, 0x00, 0xE6};

// The popup is the console's own, where the rest are the Android app's:
// the PSP's cross-media bar: a blue band with the wave through it, white type, the headword aglow.
inline const QColor PopupText{0xFF, 0xFF, 0xFF};
inline const QColor PopupDim{0xD3, 0xE4, 0xF8};
inline const QColor PopupAccent{0xFF, 0xFF, 0xFF};
/// The plus that makes a card and the book of a word Anki has, in their boxes.
inline const QColor PopupSign{0xFF, 0xFF, 0xFF};
inline const QColor PopupBookSign{0xFF, 0xFF, 0xFF};
inline const QColor PopupBox{0x1E, 0x46, 0x96};
inline const QColor PopupBoxPressed{0x3C, 0x78, 0xD2};
inline const QColor PopupEdge{0xFF, 0xFF, 0xFF, 0x99};

/// Fonts that have Japanese glyphs of their own, so that kanji are not drawn in a Chinese
/// font's shapes on a system whose language is not Japanese.
[[nodiscard]] QStringList JapaneseFamilies();
/// An open book, the reader's sign (a word Anki has; a game that can be read): two pages
/// that meet at the spine, drawn into `area` with lines `width` thick.
void DrawOpenBook(QPainter& painter, const QRectF& area, const QColor& colour, qreal width);
/// The popup's ground and its edge, the rule between two entries, and what lies behind an
/// entry (`head` is its headword's line; `first` the entry at the top).
void PaintPopupGround(QPainter& painter, const QRect& area);
void PaintPopupEdge(QPainter& painter, const QRect& area);
void PaintPopupRule(QPainter& painter, const QRect& rule);
void PaintPopupEntry(QPainter& painter, const QRectF& entry, const QRectF& head, bool first);
/// The console's pointer, `size` across, pointing at its own centre; the same as a mouse
/// pointer; and how large it is over a window.
void DrawPointer(QPainter& painter, const QPointF& centre, qreal size);
[[nodiscard]] QCursor PointerCursor(qreal size, qreal device_pixel_ratio);
[[nodiscard]] qreal PointerSize(const QWidget* over);
} // namespace Style

/**
 * What the reader draws over the game's picture: a mark on the word being looked up, the
 * session line in the corner, the cursor, a short message now and then. Nothing else: the
 * game's own text is what is read.
 *
 * The game is drawn by the graphics card into a window of its own, which nothing in the same
 * window can be drawn over. So this is a window of its own as well, without a frame, see-
 * through and deaf to the mouse, kept exactly over the game's picture by the controller.
 */
class ReaderOverlay : public QWidget {
    Q_OBJECT

public:
    explicit ReaderOverlay(QWidget* parent);

    /// Where the word being looked up is, in this window's coordinates.
    void SetMarks(const QList<QRectF>& marks_);
    /// How the marks are drawn: "highlight" (filled), "outline" (framed) or "none".
    void SetMarkStyle(const QString& style);
    /// What can be clicked, drawn to check that it sits on the game's text (a setting).
    void SetOutlines(const QList<ReaderApp::PlacedText>& texts);
    /// The session so far, shown in the top right corner of `picture`. Empty for none.
    /// `paused`: a mark for paused statistics before the line, or alone when there is none.
    void SetSessionLine(const QString& text, const QRectF& picture, bool paused = false);
    [[nodiscard]] bool PauseMark() const {
        return session_paused;
    }
    /// The cursor: a ring at `position`, or a frame around `character` when it is on one.
    void SetCursor(bool visible, const QPointF& position, const std::optional<QRectF>& character);
    /// A few words near the top of the picture, for a few seconds.
    void ShowMessage(const QString& text, const QRectF& picture);

    /// True when there is anything to draw: the window is only shown then.
    [[nodiscard]] bool HasContent() const;

    [[nodiscard]] const QList<QRectF>& Marks() const {
        return marks;
    }
    [[nodiscard]] const QString& Message() const {
        return message;
    }
    [[nodiscard]] const QString& SessionLine() const {
        return session_line;
    }

signals:
    /// HasContent may have changed.
    void ContentChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void GlideFrame();
    /// The tube's frame made anew if the window, the picture or the CRT settings changed.
    void RefreshFrame();
    /// Where the cursor is drawn, to redraw no more than that when it moves.
    [[nodiscard]] QRect CursorArea() const;

private:
    QList<QRectF> marks;
    /// The mark as drawn, on its way to `marks`, and the window's size it was placed for.
    MarkGlide mark_glide;
    QSize marks_size;
    QTimer glide_timer;
    QElapsedTimer glide_clock;
    /// The tube's frame around the picture (CRT settings, "Frame"), under all the rest.
    CrtFrame crt_frame;
    QTimer frame_timer;
    QString mark_style{QStringLiteral("outline")};
    QList<QRectF> outline_boxes;
    QList<QRectF> outline_cells;
    QString session_line;
    bool session_paused{};
    QRectF session_picture;
    bool cursor_visible{};
    QPointF cursor_position;
    std::optional<QRectF> cursor_character;
    QString message;
    QRectF message_picture;
    QTimer message_timer;
};

} // namespace ReaderUi
