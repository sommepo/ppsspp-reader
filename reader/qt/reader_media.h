// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include "reader/desktop/anki.h"
#include "reader/desktop/dictionary.h"
#include "reader/desktop/screen_text.h"
#include "reader/desktop/settings.h"

namespace ReaderUi {

/**
 * Screenshots and audio clips of lines, for cards.
 *
 * A screenshot of each line is kept for the most recent lines so that a card made from the
 * backlog shows the scene of that line rather than the current one. Audio comes from the
 * core's history of emulated output: everything the game played, not an isolated voice.
 *
 * Main thread unless noted; encoding and file work run on a thread of their own.
 */
class MediaCapture : public QObject {
    Q_OBJECT

public:
    explicit MediaCapture(ReaderApp::ReaderSettings& settings_);
    ~MediaCapture() override;

    /**
     * Copies the game's picture into an image no wider than the given width and passes it (or
     * a null image) to the callback on the main thread. Provided by the reader's controller
     * while a game is shown.
     */
    using FrameSource =
        std::function<void(int max_width, std::function<void(const QImage&)> callback)>;
    FrameSource frame_source;

    /// Keeps a screenshot for the backlog line: at once when `immediately`, otherwise after
    /// the line had time to appear. Does nothing when the line already has one.
    void CaptureForLine(qint64 backlog_id, bool immediately);
    /// The kept screenshot of a backlog line as a JPEG, or empty.
    [[nodiscard]] QByteArray ScreenshotFor(qint64 backlog_id) const;
    /// Takes a screenshot of the current frame; the callback gets it as a JPEG, or empty.
    void CaptureNow(std::function<void(const QByteArray&)> callback);

    /**
     * Call about once a second with the newest line, whose audio began at history position
     * `start`. The history only goes back so far, and a line that is read slowly would lose
     * its audio: once a clip's worth of it has played, it is copied out and kept until
     * another line has stayed as long.
     */
    void KeepAudioOf(qint64 backlog_id, qint64 start);
    /// A line's audio: as it goes into a card, and as it can be played to the user first.
    struct Clip {
        QByteArray ogg;
        /// Interleaved stereo at the console's rate (AudioTap::SampleRate).
        std::shared_ptr<const std::vector<qint16>> samples;

        [[nodiscard]] bool IsEmpty() const {
            return ogg.isEmpty();
        }
    };
    /**
     * Encodes the emulated audio of a line between two history positions as an Ogg Opus clip.
     * `end` may be empty for "until now". The clip is limited to the configured maximum
     * length and trailing silence is removed. The callback receives an empty clip when there
     * is nothing audible, or when the audio is no longer there.
     */
    void AudioClip(qint64 backlog_id, qint64 start, std::optional<qint64> end,
                   std::function<void(const Clip&)> callback);

private:
    void CaptureInto(qint64 backlog_id);
    [[nodiscard]] QString FileFor(qint64 backlog_id) const;
    [[nodiscard]] qint64 ClipStart(qint64 start) const;

    ReaderApp::ReaderSettings& settings;
    QString directory;
    QThreadPool background;
    QHash<qint64, QTimer*> pending;
    /// Lines that have a screenshot, oldest first.
    QList<qint64> captured;

    /// The audio of a line, copied out of the history before the history moved on.
    struct Held {
        qint64 backlog_id{-1};
        qint64 from{};
        std::vector<qint16> samples;
    };
    mutable QMutex held_mutex;
    Held held;
    qint64 holding{-1};
};

/**
 * Makes an Anki card from a line: gathers the screenshot and audio the note's fields ask for,
 * stores them in Anki, then adds the note. Reading statistics and the game are not affected.
 * Main thread; Anki is talked to on threads of its own.
 */
class Miner : public QObject {
    Q_OBJECT

public:
    struct Outcome {
        /// Cancelled: the user was asked about the audio and called the card off.
        enum class Kind { Added, Exists, Failed, Cancelled };
        Kind kind{Kind::Failed};
        /// Added: the new note. Exists: the notes Anki has already.
        QList<qint64> notes;
        /// Failed: what went wrong.
        QString message;
        /// Added: the field VN Library's media add-on wanted a web address in, when the card
        /// went through it and this was not known yet. It is remembered for the next card.
        QString address_field;
    };

    explicit Miner(QObject* parent = nullptr);
    ~Miner() override;

    /**
     * Puts the question of a game whose cards ask about their audio: does this card get
     * `clip`, what the game played during its line? `answer` takes true to keep it, false to
     * make the card without it, nothing to make no card. Main thread.
     */
    using AudioAnswer = std::function<void(std::optional<bool> keep)>;
    using AudioQuestion = std::function<void(const MediaCapture::Clip& clip, AudioAnswer answer)>;

    /**
     * Adds a card for `presented`, about `word` when one was looked up (its place in the line
     * is [first, last]). Where the card's audio is to be asked about, `question` is put
     * first (without one the audio is added). The callback runs on the main thread.
     */
    void Mine(const ReaderApp::PresentedLine& presented,
              const std::optional<ReaderApp::LookupResult>& word, int first, int last,
              AudioQuestion question, std::function<void(const Outcome&)> callback);
    /**
     * Asks Anki which notes it has for `word` already, the way a card made from it now would
     * be recognised: by the note type's first field, in the deck cards go to (or anywhere,
     * when the user chose that). The callback gets their ids on the main thread, an empty
     * list when there are none, or nothing when that cannot be told.
     */
    void FindExisting(const ReaderApp::PresentedLine& presented, const ReaderApp::LookupResult& word,
                      int first, int last,
                      std::function<void(const std::optional<QList<qint64>>&)> callback);
    /// Opens Anki's browser on `notes`; the callback gets an empty text, or what went wrong.
    void ShowNotes(const QList<qint64>& notes, std::function<void(const QString&)> callback);

    /// Forgets what was learned from Anki: after the setup changed.
    static void Forget();

private:
    QThreadPool worker;
    /// Questions about what Anki has do not wait behind a card that is being sent.
    QThreadPool checker;
};

/**
 * Plays a line's audio to the user before it goes into a card, through a sound device of its
 * own beside the game's. Main thread.
 */
class ClipPlayer : public QObject {
    Q_OBJECT

public:
    explicit ClipPlayer(QObject* parent = nullptr);
    ~ClipPlayer() override;

    /// Plays `samples` (interleaved stereo at the console's rate) from the start. False when
    /// no sound device could be opened.
    bool Play(std::shared_ptr<const std::vector<qint16>> samples);
    void Stop();
    [[nodiscard]] bool IsPlaying() const {
        return device != 0;
    }

signals:
    /// Playing began, or ended (by itself or because it was stopped).
    void PlayingChanged(bool playing);

private:
    void Poll();

    unsigned int device{};
    std::shared_ptr<const std::vector<qint16>> playing;
    QTimer poll;
};

} // namespace ReaderUi
