// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include "reader/desktop/glossary.h"
#include "reader/desktop/text_line.h"
#include "reader/qt/lookup_popup.h"
#include "reader/qt/reader_overlay.h"

namespace ReaderUi {

using ReaderApp::EscapeHtml;
using ReaderApp::LookupResult;
using ReaderApp::TermEntry;

namespace {

constexpr int PadLeft = 7;
constexpr int PadTop = 5;
constexpr int PadRight = 5;
constexpr int PadBottom = 4;
constexpr int EntryTop = 3;
constexpr int EntryBottom = 4;
constexpr int BoxGap = 6;
constexpr double HeadPx = 17;
constexpr double SensePx = 13;
constexpr double SmallPx = 11;
constexpr double BoxPx = 24;
constexpr int NormalMaxWidth = 420;

constexpr int KindAdd = 1;
constexpr int KindBook = 2;
constexpr int KindLink = 3;

QString Hex(const QColor& colour) {
    return colour.name(QColor::HexRgb);
}

QString Span(const QString& text, int pixels, const QColor& colour, bool bold = false) {
    return QStringLiteral("<span style=\"font-size:%1px;color:%2;%3\">%4</span>")
        .arg(pixels)
        .arg(Hex(colour), bold ? QStringLiteral("font-weight:600;") : QString(), text);
}

/// JMdict lists a word's other spellings as an entry of its own; that is not a sense.
bool IsFormsList(const TermEntry& entry) {
    return entry.definition_tags.split(u' ').contains(QStringLiteral("forms"));
}

} // namespace

LookupPopup::LookupPopup(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                          Qt::NoDropShadowWindowHint |
                          // As the layer over the game (reader_overlay.cpp): kept from the window
                          // manager with READER_OVERLAY_UNMANAGED=1, so it stands where it is put.
                          (qEnvironmentVariableIntValue("READER_OVERLAY_UNMANAGED") != 0
                               ? Qt::X11BypassWindowManagerHint
                               : Qt::WindowFlags())) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
}

LookupPopup::~LookupPopup() = default;

void LookupPopup::SetTextScale(int percent) {
    text_scale = std::clamp(percent, 50, 250);
}

void LookupPopup::SetCardCorner(Corner corner) {
    card_corner = corner;
}

LookupPopup::Corner LookupPopup::CornerOf(const QString& value) {
    if (value == QStringLiteral("bottom_right")) {
        return Corner::BottomRight;
    }
    if (value == QStringLiteral("top_left")) {
        return Corner::TopLeft;
    }
    if (value == QStringLiteral("top_right")) {
        return Corner::TopRight;
    }
    return Corner::BottomLeft;
}

void LookupPopup::SetAllowDuplicates(bool allow) {
    allow_duplicates = allow;
}

int LookupPopup::Scaled(double value) const {
    return std::max(1, static_cast<int>(std::lround(value * text_scale / 100.0)));
}

int LookupPopup::MaxWidth() const {
    return Scaled(NormalMaxWidth);
}

QFont LookupPopup::BaseFont() const {
    QFont base = font();
    base.setFamilies(Style::JapaneseFamilies());
    base.setPixelSize(Scaled(SensePx));
    return base;
}

void LookupPopup::Clear() {
    entries.clear();
    scroll = 0;
    pressed_index = -1;
    pressed_kind = 0;
}

/// The word, its reading, how it was inflected, and what frequency and pitch lists say.
QString LookupPopup::Headline(const LookupResult& result) const {
    const int small = Scaled(SmallPx);
    QString out = Span(EscapeHtml(result.expression), Scaled(HeadPx), Style::PopupText, true);
    const auto add_small = [&out, small](const QString& text, const QColor& colour) {
        out += Span(QStringLiteral("&nbsp;&nbsp;") + EscapeHtml(text), small, colour);
    };
    if (!result.DisplayReading().isEmpty()) {
        add_small(result.DisplayReading(), Style::PopupDim);
    }
    if (!result.reasons.isEmpty()) {
        add_small(QStringLiteral("« ") + result.reasons.join(QStringLiteral(" « ")), Style::PopupAccent);
    }
    for (const ReaderApp::PitchAccent& pitch : result.pitches) {
        QStringList positions;
        for (const int position : pitch.positions) {
            positions.append(QString::number(position));
        }
        add_small(u'[' + positions.join(u',') + u']', Style::PopupAccent);
    }
    for (const ReaderApp::Frequency& frequency : result.frequencies) {
        add_small(frequency.dictionary + u' ' + frequency.display, Style::PopupDim);
    }
    return out;
}

/**
 * One line per sense: its number, its tags where they differ from the sense before, its
 * glosses run together, and the dictionary's notes on it. With entries from several
 * dictionaries, each dictionary's senses are headed by its name.
 */
QString LookupPopup::Senses(const LookupResult& result) const {
    const int small = Scaled(SmallPx);
    const int sense = Scaled(SensePx);
    QList<TermEntry> shown;
    QStringList dictionaries;
    for (const TermEntry& entry : result.entries) {
        if (!IsFormsList(entry)) {
            shown.append(entry);
            if (!dictionaries.contains(entry.dictionary)) {
                dictionaries.append(entry.dictionary);
            }
        }
    }
    const bool several = dictionaries.size() > 1;
    const auto dim = [small](const QString& text) {
        return Span(EscapeHtml(text), small, Style::PopupDim);
    };
    QString out;
    QString dictionary;
    bool first = true;
    int number = 0;
    QString last_tags;
    for (const TermEntry& entry : shown) {
        const QList<ReaderApp::Glossary::Compact> definitions =
            ReaderApp::Glossary::CompactDefinitions(entry.glossary_json);
        if (definitions.isEmpty()) {
            continue;
        }
        if (!out.isEmpty()) {
            out += QStringLiteral("<br>");
        }
        if (first || entry.dictionary != dictionary) {
            first = false;
            dictionary = entry.dictionary;
            number = 0;
            last_tags.clear();
            if (several) {
                out += dim(entry.dictionary) + QStringLiteral("<br>");
            }
        }
        ++number;
        // Numbers among the tags only repeat the order of the senses.
        QStringList tag_list;
        for (const QString& tag : entry.definition_tags.split(u' ', Qt::SkipEmptyParts)) {
            bool is_number = false;
            tag.toInt(&is_number);
            if (!is_number) {
                tag_list.append(tag);
            }
        }
        const QString tags = tag_list.join(u',');
        out += dim(QString::number(number) + u' ');
        if (!tags.isEmpty() && tags != last_tags) {
            out += dim(tags + u' ');
        }
        last_tags = tags;
        QStringList glosses;
        QStringList notes;
        for (const ReaderApp::Glossary::Compact& definition : definitions) {
            glosses.append(definition.glosses);
            if (!definition.notes.isEmpty()) {
                notes.append(definition.notes);
            }
        }
        out += Span(EscapeHtml(glosses.join(QStringLiteral(" | "))), sense, Style::PopupText);
        if (!notes.isEmpty()) {
            out += dim(QStringLiteral(" (") + notes.join(QStringLiteral("; ")) + u')');
        }
    }
    return out;
}

void LookupPopup::ShowResults(const QList<LookupResult>& results) {
    Clear();
    for (const LookupResult& result : results) {
        Entry entry;
        entry.is_result = true;
        entry.document = std::make_unique<QTextDocument>();
        entry.document->setDocumentMargin(0);
        entry.document->setDefaultFont(BaseFont());
        QString html = Headline(result);
        const QString senses = Senses(result);
        if (!senses.isEmpty()) {
            html += QStringLiteral("<br>") + senses;
        }
        entry.document->setHtml(html);
        entries.push_back(std::move(entry));
    }
}

void LookupPopup::ShowMessage(const QString& message, bool offer_dictionaries) {
    Clear();
    const auto add = [this](const QString& html, bool link) {
        Entry entry;
        entry.is_link = link;
        entry.document = std::make_unique<QTextDocument>();
        entry.document->setDocumentMargin(0);
        entry.document->setDefaultFont(BaseFont());
        entry.document->setHtml(html);
        entries.push_back(std::move(entry));
    };
    add(Span(EscapeHtml(message), Scaled(SensePx), Style::PopupText), false);
    if (offer_dictionaries) {
        add(Span(EscapeHtml(tr("Open the dictionaries")), Scaled(SensePx), Style::PopupAccent), true);
    }
}

LookupPopup::CardState LookupPopup::GetCardState(int index) const {
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return CardState::Add;
    }
    return entries[static_cast<std::size_t>(index)].state;
}

void LookupPopup::SetCardState(int index, CardState state) {
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return;
    }
    entries[static_cast<std::size_t>(index)].state = state;
    if (last_max_width > 0) {
        // What a box offers can change the room the text has, and the popup's size with it.
        const QSize size = Fit(last_max_width, last_max_height);
        if (size != this->size()) {
            resize(size);
        }
    }
    update();
}

int LookupPopup::BoxCount(const Entry& entry) const {
    if (!entry.is_result) {
        return 0;
    }
    const bool in_deck = entry.state == CardState::InDeck;
    return (in_deck ? 1 : 0) + (in_deck && !allow_duplicates ? 0 : 1);
}

void LookupPopup::Layout(int max_width, int max_height) {
    last_max_width = max_width;
    last_max_height = max_height;
    const int available = std::max(60, max_width - PadLeft - PadRight);
    const int box = Scaled(BoxPx);

    // As wide as what it says, and no wider than it may be.
    int natural = 0;
    for (Entry& entry : entries) {
        entry.document->setTextWidth(-1);
        int width = static_cast<int>(std::ceil(entry.document->idealWidth()));
        width += BoxCount(entry) * (box + BoxGap);
        natural = std::max(natural, width);
    }
    content_width = std::clamp(natural, 60, available);

    int y = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Entry& entry = entries[i];
        if (i > 0) {
            y += 1; // the line between two headwords
        }
        entry.top = y;
        entry.text_top = EntryTop;
        const int boxes = BoxCount(entry);
        const int boxes_width = boxes * (box + BoxGap);
        const bool top = card_corner == Corner::TopLeft || card_corner == Corner::TopRight;
        const bool left = card_corner == Corner::TopLeft || card_corner == Corner::BottomLeft;
        // Top left: the headline begins after the boxes (its first line is indented).
        {
            QTextCursor cursor{entry.document->firstBlock()};
            QTextBlockFormat format = cursor.blockFormat();
            const qreal indent = card_corner == Corner::TopLeft && boxes > 0 ? boxes_width : 0;
            if (format.textIndent() != indent) {
                format.setTextIndent(indent);
                cursor.setBlockFormat(format);
            }
        }
        entry.document->setTextWidth(content_width);
        const int text_height = static_cast<int>(std::ceil(entry.document->size().height()));
        int height = EntryTop + text_height + EntryBottom;
        entry.add_box = QRect();
        entry.book_box = QRect();
        if (boxes > 0) {
            // The boxes stand in the chosen corner. Bottom left: a row of their own under the
            // text. Bottom right and top right: beside the last or the first line, or a row of
            // their own where that line leaves them no room. Top left: before the headline.
            bool own_row = card_corner == Corner::BottomLeft;
            if (card_corner == Corner::BottomRight || card_corner == Corner::TopRight) {
                const QTextBlock block = top ? entry.document->firstBlock() : entry.document->lastBlock();
                qreal line = 0;
                if (const QTextLayout* layout = block.layout(); layout && layout->lineCount() > 0) {
                    line = layout->lineAt(top ? 0 : layout->lineCount() - 1).naturalTextWidth();
                }
                own_row = line + boxes_width > content_width;
            }
            if (own_row) {
                height += box + 2;
                if (top) {
                    entry.text_top += box + 2;
                }
            } else {
                height = std::max(height, EntryTop + box + EntryBottom);
            }
            const int box_top = top ? entry.top + EntryTop - 1 : entry.top + height - EntryBottom + 1 - box;
            // The plus is the one nearer the corner, the book beside it.
            int x = left ? 0 : content_width - box;
            const int step = left ? box + BoxGap : -(box + BoxGap);
            const bool in_deck = entry.state == CardState::InDeck;
            if (!in_deck || allow_duplicates) {
                entry.add_box = QRect(x, box_top, box, box);
                x += step;
            }
            if (in_deck) {
                entry.book_box = QRect(x, box_top, box, box);
            }
        }
        entry.height = height;
        y += height;
    }
    content_height = y;
    scroll = std::clamp(scroll, 0, std::max(0, PadTop + content_height + PadBottom - max_height));
}

QSize LookupPopup::Fit(int max_width, int max_height) {
    Layout(max_width, max_height);
    return QSize(PadLeft + content_width + PadRight,
                 std::min(PadTop + content_height + PadBottom, std::max(max_height, 1)));
}

int LookupPopup::MaxScroll() const {
    return std::max(0, PadTop + content_height + PadBottom - height());
}

void LookupPopup::ScrollTo(int y) {
    const int next = std::clamp(y, 0, MaxScroll());
    if (next != scroll) {
        scroll = next;
        update();
    }
}

QRect LookupPopup::AddBox(int index) const {
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return {};
    }
    const QRect box = entries[static_cast<std::size_t>(index)].add_box;
    return box.isNull() ? box : box.translated(PadLeft, PadTop - scroll);
}

QRect LookupPopup::BookBox(int index) const {
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return {};
    }
    const QRect box = entries[static_cast<std::size_t>(index)].book_box;
    return box.isNull() ? box : box.translated(PadLeft, PadTop - scroll);
}

QString LookupPopup::PlainText() const {
    QStringList parts;
    for (const Entry& entry : entries) {
        parts.append(entry.document->toPlainText());
    }
    return parts.join(u'\n');
}

int LookupPopup::HitTest(const QPoint& point, int& index) const {
    const QPoint content = point - QPoint(PadLeft, PadTop - scroll);
    // A little beyond the box it shows, up and to the left of it.
    const auto grown = [](const QRect& box) { return box.adjusted(-6, -6, 2, 2); };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        index = static_cast<int>(i);
        if (!entry.add_box.isNull() && grown(entry.add_box).contains(content)) {
            return KindAdd;
        }
        if (!entry.book_box.isNull() && grown(entry.book_box).contains(content)) {
            return KindBook;
        }
        if (entry.is_link &&
            QRect(0, entry.top, content_width, entry.height).contains(content)) {
            return KindLink;
        }
    }
    index = -1;
    return 0;
}

void LookupPopup::DrawBox(QPainter& painter, const QRect& box, bool book, bool pressed_,
                          bool enabled) const {
    const QColor colour = book ? Style::PopupBookSign : Style::PopupSign;
    QColor edge = colour;
    edge.setAlpha(0xA0);
    painter.setOpacity(enabled ? 1.0 : 0.4);
    painter.setPen(QPen(edge, 1));
    // Filled, not see-through: text may pass under a box.
    painter.setBrush(pressed_ ? Style::PopupBoxPressed : Style::PopupBox);
    painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    const QRectF inner = QRectF(box).adjusted(box.width() * 0.24, box.height() * 0.24,
                                              -box.width() * 0.24, -box.height() * 0.24);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colour, std::max(1.6, box.width() / 12.0), Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    if (!book) {
        painter.drawLine(QPointF(inner.center().x(), inner.top()),
                         QPointF(inner.center().x(), inner.bottom()));
        painter.drawLine(QPointF(inner.left(), inner.center().y()),
                         QPointF(inner.right(), inner.center().y()));
    } else {
        Style::DrawOpenBook(painter, inner, colour, std::max(1.6, box.width() / 12.0));
    }
    painter.setOpacity(1.0);
}

void LookupPopup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    Style::PaintPopupGround(painter, rect());
    painter.setRenderHint(QPainter::Antialiasing);

    painter.save();
    painter.setClipRect(QRect(1, 1, width() - 2, height() - 2));
    painter.translate(PadLeft, PadTop - scroll);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        if (i > 0) {
            Style::PaintPopupRule(painter, QRect(0, entry.top - 1, content_width, 1));
        }
        if (entry.is_result) {
            Style::PaintPopupEntry(painter, QRectF(0, entry.top, content_width, entry.height),
                                   QRectF(0, entry.top + entry.text_top, content_width, Scaled(HeadPx) * 1.35),
                                   i == 0);
        }
        painter.save();
        painter.translate(0, entry.top + entry.text_top);
        entry.document->drawContents(&painter);
        painter.restore();
        const bool working = entry.state == CardState::Working;
        const int index = static_cast<int>(i);
        if (!entry.book_box.isNull()) {
            DrawBox(painter, entry.book_box, true,
                    pressed_index == index && pressed_kind == KindBook, true);
        }
        if (!entry.add_box.isNull()) {
            DrawBox(painter, entry.add_box, false,
                    pressed_index == index && pressed_kind == KindAdd, !working);
        }
    }
    painter.restore();

    // How far it is scrolled, when there is more than fits.
    const int most = MaxScroll();
    if (most > 0) {
        const qreal total = PadTop + content_height + PadBottom;
        const qreal bar = std::max(12.0, height() * height() / total);
        const qreal top = (height() - bar) * scroll / most;
        painter.setPen(Qt::NoPen);
        painter.setBrush(Style::PopupEdge);
        painter.drawRoundedRect(QRectF(width() - 4, top, 3, bar), 1.5, 1.5);
    }

    Style::PaintPopupEdge(painter, rect());
}

void LookupPopup::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed_kind = HitTest(event->position().toPoint(), pressed_index);
        update();
    }
    event->accept(); // clicks in the popup never fall through to the game
}

void LookupPopup::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int index = -1;
        const int kind = HitTest(event->position().toPoint(), index);
        const bool same = kind != 0 && kind == pressed_kind && index == pressed_index;
        pressed_kind = 0;
        pressed_index = -1;
        update();
        if (same) {
            if (kind == KindAdd) {
                if (GetCardState(index) != CardState::Working) {
                    emit AddRequested(index);
                }
            } else if (kind == KindBook) {
                emit NotesRequested(index);
            } else if (kind == KindLink) {
                emit DictionariesRequested();
            }
        }
    }
    event->accept();
}

void LookupPopup::mouseMoveEvent(QMouseEvent* event) {
    // The console's pointer here too, on a box as beside it: the system's hand and arrow are
    // another machine's.
    if (!pointer_set) {
        pointer_set = true;
        setCursor(Style::PointerCursor(Style::PointerSize(parentWidget()), devicePixelRatioF()));
    }
    event->accept();
}

void LookupPopup::wheelEvent(QWheelEvent* event) {
    ScrollTo(scroll - event->angleDelta().y() / 3);
    event->accept();
}

void LookupPopup::leaveEvent(QEvent*) {
    if (pressed_kind != 0) {
        pressed_kind = 0;
        pressed_index = -1;
        update();
    }
}

} // namespace ReaderUi
