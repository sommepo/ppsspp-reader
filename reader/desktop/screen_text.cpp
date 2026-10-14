// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include "reader/desktop/screen_text.h"

namespace ReaderApp {

namespace {

std::optional<QRectF> CellFrom(const QJsonValue& value) {
    if (!value.isArray()) {
        return std::nullopt;
    }
    const QJsonArray array = value.toArray();
    if (array.size() < 4) {
        return std::nullopt;
    }
    double values[4];
    for (int i = 0; i < 4; ++i) {
        if (!array.at(i).isDouble()) {
            return std::nullopt;
        }
        values[i] = array.at(i).toDouble();
        if (!std::isfinite(values[i])) {
            return std::nullopt;
        }
    }
    if (values[2] < 0 || values[3] < 0) {
        return std::nullopt;
    }
    return QRectF(values[0], values[1], values[2], values[3]);
}

/// The part of `rect` inside `bounds` (possibly empty).
QRectF ClampedTo(const QRectF& rect, const QRectF& bounds) {
    const qreal left = std::clamp(rect.left(), bounds.left(), bounds.right());
    const qreal top = std::clamp(rect.top(), bounds.top(), bounds.bottom());
    const qreal right = std::clamp(rect.right(), bounds.left(), bounds.right());
    const qreal bottom = std::clamp(rect.bottom(), bounds.top(), bounds.bottom());
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

/// The smallest rectangle around both, also when one of them has no area.
QRectF Union(const QRectF& a, const QRectF& b) {
    return QRectF(QPointF(std::min(a.left(), b.left()), std::min(a.top(), b.top())),
                  QPointF(std::max(a.right(), b.right()), std::max(a.bottom(), b.bottom())));
}

bool Contains(const QRectF& rect, const QPointF& point) {
    return point.x() >= rect.left() && point.x() < rect.right() && point.y() >= rect.top() &&
           point.y() < rect.bottom();
}

/// Room for a pointer left of a row's first and right of its last character, in cells.
constexpr qreal EndRoom = 0.6;
/// Room above the first row (furigana) and below the last, in row heights.
constexpr qreal RoomAbove = 0.6;
constexpr qreal RoomBelow = 0.35;

} // namespace

std::optional<ScreenLayout> ScreenLayout::FromJson(const QByteArray& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    ScreenLayout layout;
    layout.window = root.value(QStringLiteral("window")).toString();
    if (layout.window.isEmpty()) {
        return std::nullopt;
    }
    layout.sequence = root.value(QStringLiteral("seq")).toInteger();
    if (root.value(QStringLiteral("closed")).toBool()) {
        layout.closed = true;
        return layout;
    }
    if (!root.value(QStringLiteral("glyphs")).isArray()) {
        return std::nullopt;
    }
    for (const QJsonValue& glyph : root.value(QStringLiteral("glyphs")).toArray()) {
        layout.cells.append(CellFrom(glyph));
    }
    const QJsonArray space = root.value(QStringLiteral("space")).toArray();
    const auto size = [&space](int index, float fallback) {
        const double value = space.at(index).toDouble(0);
        return std::isfinite(value) && value >= 1.0 ? static_cast<float>(value) : fallback;
    };
    layout.space_width = size(0, DefaultWidth);
    layout.space_height = size(1, DefaultHeight);
    layout.box = CellFrom(root.value(QStringLiteral("box")));
    return layout;
}

QList<ScreenText> ScreenTexts::OnScreen() const {
    QList<ScreenText> out;
    for (const QString& window : shown_order) {
        out.append(shown.value(window));
    }
    return out;
}

std::optional<ScreenText> ScreenTexts::PlacementOf(const PresentedLine& presented) const {
    const auto found = shown.constFind(presented.line.window);
    if (found == shown.constEnd() || found->presented.backlog_id != presented.backlog_id) {
        return std::nullopt;
    }
    return *found;
}

bool ScreenTexts::AdoptLine(const PresentedLine& presented) {
    const QString window = presented.line.window;
    if (window.isEmpty()) {
        return false;
    }
    lines.insert(window, presented);
    // A placement shown so far was for the text this one replaces.
    bool changed = shown.remove(window) > 0;
    shown_order.removeAll(window);
    const auto early = waiting.constFind(window);
    if (early != waiting.constEnd() && early->sequence <= presented.line.sequence) {
        const ScreenLayout layout = *early;
        waiting.remove(window);
        if (layout.sequence == presented.line.sequence) {
            shown.insert(window, ScreenText{presented, layout});
            shown_order.append(window);
            changed = true;
        }
    }
    return changed;
}

bool ScreenTexts::ApplyLayout(const ScreenLayout& layout) {
    const QString window = layout.window;
    if (layout.closed) {
        waiting.remove(window);
        shown_order.removeAll(window);
        return shown.remove(window) > 0;
    }
    const auto line = lines.constFind(window);
    if (line != lines.constEnd() && line->line.sequence == layout.sequence) {
        if (!shown.contains(window)) {
            shown_order.append(window);
        }
        shown.insert(window, ScreenText{*line, layout});
        return true;
    }
    if (line == lines.constEnd() || line->line.sequence < layout.sequence) {
        // Its line is still on the way.
        waiting.insert(window, layout);
        return false;
    }
    // For text that has been replaced since.
    return false;
}

bool ScreenTexts::Clear() {
    lines.clear();
    waiting.clear();
    const bool changed = !shown.isEmpty();
    shown.clear();
    shown_order.clear();
    return changed;
}

static std::function<void(float&, float&)>& Bend() {
    static std::function<void(float&, float&)> bend;
    return bend;
}

QRectF PlaceCell(const QRectF& cell, const ScreenLayout& layout, const QRectF& area) {
    const qreal scale_x = area.width() / layout.space_width;
    const qreal scale_y = area.height() / layout.space_height;
    QRectF placed(QPointF(area.left() + cell.left() * scale_x, area.top() + cell.top() * scale_y),
                  QPointF(area.left() + cell.right() * scale_x,
                          area.top() + cell.bottom() * scale_y));
    if (Bend() && area.width() > 0 && area.height() > 0) {
        const QPointF middle = placed.center();
        float x = static_cast<float>((middle.x() - area.left()) / area.width());
        float y = static_cast<float>((middle.y() - area.top()) / area.height());
        Bend()(x, y);
        placed.moveCenter(QPointF(area.left() + x * area.width(), area.top() + y * area.height()));
    }
    return placed;
}

void PlacedText::SetBend(std::function<void(float& x, float& y)> bend) {
    Bend() = std::move(bend);
}

PlacedText::Hit PlacedText::HitTest(const QPointF& point) const {
    for (const Glyph& character : characters) {
        if (Contains(character.touch, point)) {
            return Hit{HitKind::Character, this, &character};
        }
    }
    if (Contains(box, point)) {
        return Hit{HitKind::Box, this, nullptr};
    }
    return Hit{};
}

QList<QRectF> PlacedText::Marks(int first, int last) const {
    QList<int> row_order;
    QHash<int, QRectF> by_row;
    for (const Glyph& character : characters) {
        if (character.offset < first || character.offset > last) {
            continue;
        }
        const auto found = by_row.find(character.row);
        if (found == by_row.end()) {
            by_row.insert(character.row, character.drawn);
            row_order.append(character.row);
        } else {
            *found = Union(*found, character.drawn);
        }
    }
    QList<QRectF> out;
    for (const int row : row_order) {
        out.append(by_row.value(row));
    }
    return out;
}

std::optional<PlacedText> PlacedText::Place(const PresentedLine& presented,
                                            const ScreenLayout& layout, const QRectF& area) {
    struct Cell {
        int offset;
        QRectF drawn;
    };
    const QString& text = presented.line.text;
    QList<Cell> cells;
    int offset = 0;
    int index = 0;
    while (offset < text.size() && index < layout.cells.size()) {
        const std::optional<QRectF>& cell = layout.cells.at(index);
        if (cell && cell->width() > 0 && cell->height() > 0) {
            cells.append(Cell{offset, PlaceCell(*cell, layout, area)});
        }
        offset += CharCountAt(text, offset);
        ++index;
    }
    if (cells.isEmpty()) {
        return std::nullopt;
    }

    // Rows: characters follow each other in a row as long as they stay level.
    QList<QList<Cell>> rows;
    for (const Cell& cell : cells) {
        bool level = false;
        if (!rows.isEmpty()) {
            const QRectF& last = rows.last().last().drawn;
            level = std::abs((cell.drawn.top() + cell.drawn.bottom()) -
                             (last.top() + last.bottom())) <
                    std::min(cell.drawn.height(), last.height());
        }
        if (level) {
            rows.last().append(cell);
        } else {
            rows.append(QList<Cell>{cell});
        }
    }

    PlacedText placed;
    placed.window = layout.window;
    placed.presented = presented;
    qreal previous_bottom = 0;
    for (int row_index = 0; row_index < rows.size(); ++row_index) {
        const QList<Cell>& row = rows.at(row_index);
        qreal row_top = row.first().drawn.top();
        qreal row_bottom = row.first().drawn.bottom();
        for (const Cell& cell : row) {
            row_top = std::min(row_top, cell.drawn.top());
            row_bottom = std::max(row_bottom, cell.drawn.bottom());
        }
        const qreal row_height = row_bottom - row_top;
        // The gap above a row belongs to it: that is where its furigana is.
        const qreal top = row_index == 0 ? row_top - row_height * RoomAbove
                                         : std::min(previous_bottom, row_top);
        const qreal bottom =
            row_index == rows.size() - 1 ? row_bottom + row_height * RoomBelow : row_bottom;
        for (int i = 0; i < row.size(); ++i) {
            const QRectF& drawn = row.at(i).drawn;
            const qreal left = i == 0 ? drawn.left() - drawn.width() * EndRoom
                                      : (row.at(i - 1).drawn.right() + drawn.left()) / 2.0;
            const qreal right = i == row.size() - 1
                                    ? drawn.right() + drawn.width() * EndRoom
                                    : (drawn.right() + row.at(i + 1).drawn.left()) / 2.0;
            // Never beyond the game's picture: the borders beside it are not the game's text
            // box.
            const QRectF touch =
                ClampedTo(QRectF(QPointF(left, top), QPointF(right, bottom)), area);
            placed.characters.append(Glyph{row.at(i).offset, row_index, drawn, touch});
        }
        previous_bottom = row_bottom;
    }

    placed.rows = placed.characters.first().touch;
    for (const Glyph& character : placed.characters) {
        placed.rows = Union(placed.rows, character.touch);
    }
    QRectF box = layout.box ? PlaceCell(*layout.box, layout, area) : placed.rows;
    box = Union(box, placed.rows);
    placed.box = ClampedTo(box, area);
    return placed;
}

} // namespace ReaderApp
