// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPushButton;

namespace ReaderUi {

/**
 * What a card asks before it is made, in a game whose cards ask about their audio: whether it
 * gets what the game played during its line. One line of text and three buttons, in the
 * dictionary popup's look: listen to the clip, make the card with it, make the card without
 * it. A window of its own, like the popup, that never takes the keyboard from the game.
 */
class AudioPrompt : public QWidget {
    Q_OBJECT

public:
    /// For this long after it came up, the question takes no answer: the press that asked for
    /// the card is not one.
    static constexpr int GuardMs = 300;

    explicit AudioPrompt(QWidget* parent);

    /// Makes the question ready to be shown, its text at `scale` percent of the normal size
    /// (the dictionary popup's setting).
    void Prepare(int scale);
    /// While the clip plays, the first button stops it.
    void SetPlaying(bool playing);
    [[nodiscard]] bool IsPlaying() const {
        return playing;
    }

signals:
    void ListenClicked();
    void KeepClicked();
    void DropClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QLabel* question;
    QPushButton* listen;
    QPushButton* keep;
    QPushButton* drop;
    QElapsedTimer shown;
    bool playing{};
};

} // namespace ReaderUi
