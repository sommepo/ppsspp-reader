// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QList>
#include <QRect>
#include <QString>
#include <QWidget>
#include "reader/desktop/dictionary.h"

class QTextDocument;

namespace ReaderUi {

/**
 * The dictionary popup, kept as small as it can be: it is only as wide and as tall as what it
 * says. Per headword there is one line with the word, its reading and how it was inflected,
 * then one line per sense with its glosses run together. In the bottom right corner of each
 * headword's part sits a boxed button: a plus to make a card, or an open book when Anki has
 * the word already.
 *
 * A window of its own, without a frame, that never takes the keyboard from the game.
 */
class LookupPopup : public QWidget {
    Q_OBJECT

public:
    /// What the box at the foot of a headword offers.
    enum class CardState {
        Add,     ///< Not in Anki, as far as is known: a plus that makes a card.
        Working, ///< A card is being made.
        InDeck   ///< Anki has the word: an open book that shows its notes.
    };

    explicit LookupPopup(QWidget* parent);
    ~LookupPopup() override;

    /// Size of the popup's text relative to its normal size, in percent.
    void SetTextScale(int percent);
    /// Keep offering the plus beside the book for a word Anki already has.
    void SetAllowDuplicates(bool allow);

    void ShowResults(const QList<ReaderApp::LookupResult>& results);
    void ShowMessage(const QString& message, bool offer_dictionaries);

    /// What the box of result `index` offers.
    [[nodiscard]] CardState GetCardState(int index) const;
    void SetCardState(int index, CardState state);
    [[nodiscard]] int ResultCount() const {
        return static_cast<int>(entries.size());
    }

    /// Lays the content out for a popup no larger than this, and returns the size it takes.
    [[nodiscard]] QSize Fit(int max_width, int max_height);
    /// How far the content is scrolled, and how far it can be.
    [[nodiscard]] int Scrolled() const {
        return scroll;
    }
    void ScrollTo(int y);
    /// Where the boxes of result `index` are, in the popup's coordinates. For tests and for
    /// the cursor's clicks.
    [[nodiscard]] QRect AddBox(int index) const;
    [[nodiscard]] QRect BookBox(int index) const;
    /// What the popup says, as plain text. For tests.
    [[nodiscard]] QString PlainText() const;

    /// The largest width the popup takes at the current text scale.
    [[nodiscard]] int MaxWidth() const;

signals:
    void AddRequested(int index);
    void NotesRequested(int index);
    void DictionariesRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool pointer_set{};

    struct Entry {
        std::unique_ptr<QTextDocument> document;
        CardState state{CardState::Add};
        /// In content coordinates (before scrolling).
        int top{};
        int height{};
        QRect add_box;
        QRect book_box;
        bool is_result{};
        bool is_link{};
    };

    void Clear();
    [[nodiscard]] QFont BaseFont() const;
    [[nodiscard]] int Scaled(double value) const;
    [[nodiscard]] QString Headline(const ReaderApp::LookupResult& result) const;
    [[nodiscard]] QString Senses(const ReaderApp::LookupResult& result) const;
    [[nodiscard]] int BoxCount(const Entry& entry) const;
    void Layout(int max_width, int max_height);
    /// Which box of which entry is at a point of the popup: 1 add, 2 book, 3 link.
    [[nodiscard]] int HitTest(const QPoint& point, int& index) const;
    [[nodiscard]] int MaxScroll() const;
    void DrawBox(QPainter& painter, const QRect& box, bool book, bool pressed_, bool enabled) const;

    std::vector<Entry> entries;
    int text_scale{100};
    bool allow_duplicates{};
    int content_width{};
    int content_height{};
    int scroll{};
    int last_max_width{};
    int last_max_height{};
    int pressed_index{-1};
    int pressed_kind{};
};

} // namespace ReaderUi
