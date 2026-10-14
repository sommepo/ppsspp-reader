// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include "reader/desktop/dictionary.h"
#include "reader/desktop/screen_text.h"
#include "reader/qt/audio_prompt.h"
#include "reader/qt/lookup_popup.h"
#include "reader/qt/reader_media.h"

namespace ReaderUi {

/**
 * Dictionary lookups over some text: runs the lookup for a character, shows the popup beside
 * the text, says which word was matched so that it can be marked, and makes cards from the
 * results. Used for the game's own text and for the backlog; each has one of its own.
 *
 * Main thread. The dictionary is searched on a thread of this object's.
 */
class LookupFlow : public QObject {
    Q_OBJECT

public:
    /**
     * Where the popup belongs, in the coordinates of `bounds`' owner: above or below
     * `keep_clear` (the text the word is in, which the popup must not cover), starting near
     * `near_x` (the word), and inside `bounds`.
     */
    struct Anchor {
        QRect bounds;
        QRect keep_clear;
        int near_x{};
        /// The widget those coordinates are in: the popup is placed on screen from them.
        QWidget* owner{};
    };

    /// `popup_parent` is the window the popup belongs to and stays above.
    explicit LookupFlow(QWidget* popup_parent);
    ~LookupFlow() override;

    /// Marks [first, last] of the line `backlog_id`'s text, or takes the mark away (-1).
    std::function<void(qint64 backlog_id, int first, int last)> mark;
    /// Says a few words to the user: how a card request ended.
    std::function<void(const QString&)> notify;
    /// The popup's "open the dictionaries" was pressed.
    std::function<void()> open_dictionaries;

    /**
     * Looks up the word starting at `offset` of `presented`'s text and shows what was found
     * at `beside`. A `quiet` lookup that finds no word changes nothing: for lookups nobody
     * asked for by hand, such as under a pointer passing over punctuation.
     */
    void Lookup(const ReaderApp::PresentedLine& presented, int offset, const Anchor& beside,
                bool quiet = false);
    void Close();
    /// Makes a card of `presented` alone, without a word.
    void MineSentence(const ReaderApp::PresentedLine& presented);

    [[nodiscard]] bool IsOpen() const;
    /// True when the open popup is about the character at `offset` of the line `backlog_id`.
    [[nodiscard]] bool IsAbout(qint64 backlog_id, int offset) const;
    /**
     * The pointer moved: the mouse, or the controller's cursor. `inside`: it is on the popup
     * or on the popup's word. Inside, the popup's time starts over; outside, the popup goes
     * away after a moment (long enough to cross from the word to the popup) when the settings
     * say so. The mouse on the popup itself is seen here.
     */
    void PointerMoved(bool inside);
    /// Asked when the popup comes up and when its time is up: whether the pointer is on the
    /// popup or on its word right now (it may have been resting there without moving).
    std::function<bool()> pointer_inside_now;
    /// How long the pointer may be off the word and the popup before the popup goes.
    static constexpr int ExitGraceMs = 450;
    [[nodiscard]] LookupPopup* Popup() const {
        return popup.data();
    }
    /// True while a card's question about its audio is up.
    [[nodiscard]] bool IsAsking() const {
        return !asking.isEmpty();
    }
    /// Calls off every card that is waiting for an answer about its audio.
    void CancelQuestions();
    [[nodiscard]] AudioPrompt* Prompt() const {
        return prompt.data();
    }
    /// Where a question goes that belongs to no popup (a card of the sentence): the middle
    /// of this widget.
    void SetHome(QWidget* widget) {
        home = widget;
    }

    /// The anchor has moved (the window was moved or resized): the popup follows.
    void Reposition(const Anchor& beside);
    /// The window the popup belongs to changed (the game went to full screen).
    void SetPopupParent(QWidget* parent);

signals:
    /// The popup went away, however that came about.
    void Closed();
    /// A lookup's popup came up.
    void Opened();

private:
    struct Pinned {
        ReaderApp::PresentedLine presented;
        int offset{};
        QList<ReaderApp::LookupResult> results;
        /// The notes Anki has for the words of the open popup, as far as it has answered.
        QHash<int, QList<qint64>> notes;
    };

    void OnLookupDone(int request, const ReaderApp::PresentedLine& presented, int offset,
                      bool quiet, bool has_dictionaries,
                      const QList<ReaderApp::LookupResult>& results);
    void AskAnki();
    void Mine(int index);
    void ShowNotes(int index);
    void Report(const Miner::Outcome& outcome);
    void Position();
    void Ask(const MediaCapture::Clip& clip, Miner::AudioAnswer answer);
    void ShowQuestion();
    void PlaceQuestion();
    void AnswerQuestion(std::optional<bool> keep);

    /// Its window's, and gone with it: the pointer says whether it is still there.
    QPointer<LookupPopup> popup;
    Miner miner;
    QThreadPool worker;
    std::optional<Pinned> pinned;
    bool eventFilter(QObject* watched, QEvent* event) override;
    /// Starts the popup's time over: it closes by itself when left alone for the time in the
    /// settings.
    void KeepOpen();
    /// The word the open popup is about, also when nothing was found for it.
    qint64 word_line{-1};
    int word_first{-1};
    int word_last{-1};
    bool pointer_inside{};
    QTimer hide_timer;
    QTimer exit_timer;
    Anchor anchor;
    /// Counts what the popup has shown: answers about an earlier content are dropped.
    int token{};
    /// Counts lookups: only the latest one's result is shown.
    int asked{};

    /// A card that waits for the answer about its audio.
    struct Asked {
        MediaCapture::Clip clip;
        Miner::AudioAnswer answer;
    };
    /// One question is shown at a time; the others wait their turn.
    QList<Asked> asking;
    QPointer<AudioPrompt> prompt;
    ClipPlayer player;
    QPointer<QWidget> home;
};

} // namespace ReaderUi
