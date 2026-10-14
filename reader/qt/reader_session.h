// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include "reader/core/types.h"
#include "reader/desktop/dictionary.h"
#include "reader/desktop/reader_database.h"
#include "reader/desktop/screen_text.h"
#include "reader/desktop/settings.h"
#include "reader/desktop/stats.h"
#include "reader/desktop/text_hook_server.h"
#include "reader/desktop/text_line.h"

namespace ReaderUi {

class MediaCapture;

/// Whether a text profile is active for the running game.
struct ProfileStatus {
    QString title_id;
    bool loaded{};
    int hooks{};
    QString error;
};

/**
 * The reading session of the running game, on the desktop: receives text from the core, counts
 * it, keeps the backlog and feeds everything that shows or forwards text. The desktop sibling
 * of the Android app's ReaderSession.
 *
 * Lives on the app's main thread, and its signals are emitted there. Database and counting
 * work happens on one worker thread, in arrival order.
 */
class ReaderSession : public QObject {
    Q_OBJECT

public:
    static ReaderSession& Instance();

    // ---- What the reader keeps, whether a game runs or not ----

    [[nodiscard]] ReaderApp::ReaderSettings& Settings();
    [[nodiscard]] ReaderApp::ReaderDatabase& Database();
    /// Runs `task` on the session's worker, after what a game that just stopped still has to
    /// write of its statistics.
    void AfterWrites(std::function<void()> task);
    [[nodiscard]] ReaderApp::DictionaryDatabase& Dictionaries();
    [[nodiscard]] ReaderApp::DictionaryLookup& Lookup();
    /// The folder the reader keeps its files in (inside the app's own).
    [[nodiscard]] static QString DataDirectory();

    // ---- The running game ----

    /// Starts a session for the game that was just loaded.
    void Start(const QString& game_id, const QString& title);
    /// Ends the session and saves pending activity.
    void Stop();
    /// True while the game is running in front of the user.
    void SetReading(bool value);
    /// Re-reads the settings that affect a running session.
    void OnSettingsChanged();
    /// The user did something that shows they are still reading.
    void NoteInteraction();
    /// Forgets the tracker's open session after sessions were deleted or cleared.
    void OnActivityCleared();
    /// Saves pending activity now, then runs `then` on the main thread.
    void Flush(std::function<void()> then = {});

    [[nodiscard]] bool IsActive() const {
        return !title_id.isEmpty();
    }
    [[nodiscard]] const QString& GameKey() const {
        return title_id;
    }
    [[nodiscard]] const QString& GameTitle() const {
        return title;
    }
    [[nodiscard]] const ProfileStatus& Profile() const {
        return profile_status;
    }
    /// The line most recently presented.
    [[nodiscard]] const std::optional<ReaderApp::PresentedLine>& Current() const {
        return current;
    }
    /// The latest choice list, shown beside the current line.
    [[nodiscard]] const std::optional<ReaderApp::PresentedLine>& CurrentChoices() const {
        return current_choices;
    }
    /// The lines whose text the game is showing, with where each character is.
    [[nodiscard]] QList<ReaderApp::ScreenText> OnScreen() const {
        return screen.OnScreen();
    }
    [[nodiscard]] std::optional<ReaderApp::ScreenText> PlacementOf(
        const ReaderApp::PresentedLine& presented) const {
        return screen.PlacementOf(presented);
    }
    /// What the reading session being added to holds so far. Nothing until a line was counted.
    [[nodiscard]] const std::optional<ReaderApp::ReadingCounters>& SessionActivity() const {
        return session_activity;
    }
    /// Audio history position where the line after `backlog_id` began, or nothing when that
    /// line is the newest one (its audio runs until now).
    [[nodiscard]] std::optional<qint64> AudioEndFor(qint64 backlog_id) const;
    /// A line of this run by its backlog id, with its audio position.
    [[nodiscard]] std::optional<ReaderApp::PresentedLine> RecentLine(qint64 backlog_id) const;
    /// Screenshots and audio for cards. Null while no game is running.
    [[nodiscard]] MediaCapture* Media() const {
        return media.get();
    }
    /// Address other programs can connect to, or empty when the text server is off.
    [[nodiscard]] QString TextHookAddress() const;

    // ---- From the core, on whatever thread it reports from ----

    void PostLine(ReaderApp::TextLine line);
    void PostLayout(ReaderApp::ScreenLayout layout);
    void PostLog(int level, QString message);
    void PostProfileStatus(std::string game_id, bool loaded, int hooks, std::string message);

signals:
    void LinePresented(const ReaderApp::PresentedLine& presented);
    void ProfileStatusChanged();
    /// Totals in the database changed.
    void ActivityChanged();
    /// What OnScreen returns changed: text appeared on screen, moved or went away.
    void ScreenTextChanged();
    void SessionActivityChanged();

private:
    ReaderSession();
    ~ReaderSession() override;

    void OpenData();
    void HandleLine(const ReaderApp::TextLine& line, qint64 now); // worker thread
    void PublishActivity();                                         // worker thread
    void FlushTracker();                                            // worker thread
    void Present(const ReaderApp::PresentedLine& presented);
    void AdoptWindowLine(const ReaderApp::PresentedLine& presented);
    void ClearScreenTexts();
    void ApplyTextHookSettings();
    void Heartbeat();
    /// Runs `task` on the worker thread, after everything queued before it.
    void OnWorker(std::function<void()> task);

    std::unique_ptr<ReaderApp::ReaderSettings> settings;
    std::unique_ptr<ReaderApp::ReaderDatabase> database;
    std::unique_ptr<ReaderApp::DictionaryDatabase> dictionaries;
    std::unique_ptr<ReaderApp::DictionaryLookup> lookup;

    QThread worker_thread;
    QObject* worker_context{};
    // Worker thread only.
    std::unique_ptr<ReaderApp::ActivityTracker> tracker;
    std::optional<ReaderApp::TextLine> last_line;
    std::optional<ReaderApp::PresentedLine> last_presented;
    std::optional<ReaderApp::ReadingCounters> published_activity;
    qint64 last_flush{};
    QString worker_title_id;

    // Main thread only.
    QString title_id;
    QString title;
    bool reading{};
    ProfileStatus profile_status;
    ReaderApp::ScreenTexts screen;
    std::optional<ReaderApp::PresentedLine> current;
    std::optional<ReaderApp::PresentedLine> current_choices;
    std::optional<ReaderApp::ReadingCounters> session_activity;
    /// Recent narrative lines, oldest first, to find where a line's audio ends.
    QList<ReaderApp::PresentedLine> recent;
    std::unique_ptr<MediaCapture> media;
    std::unique_ptr<ReaderApp::TextHookServer> hook_server;
    QTimer heartbeat;
    /// Counts sessions: what the worker sends back for an earlier one is dropped.
    std::atomic<quint64> generation{};
};

/// What the emulator's frontend calls around a game, and what the text engine is given.
namespace ReaderBridge {
/// A game is about to be loaded: the text engine reports to the app from here on.
/// Once, on the main thread: reads what the emulator's threads will ask for later (the
/// bundled profiles, where the user's are), since those threads may not use Qt.
void PrepareFiles();
void OnGameLoading();
/// The game is loaded: its text profile goes in before any of its code has run.
void OnGameStarted();
void OnGameStopped();
/// Reloads the running game's profile at the next emulated frame.
void ReloadProfile();
/// The profile the game `title_id` is read with: a file in the app's folder under
/// reader/profiles wins over the one bundled with the app. Nothing when there is none.
[[nodiscard]] std::optional<QByteArray> ProfileSource(const QString& title_id);
[[nodiscard]] bool HasUserProfile(const QString& title_id);
[[nodiscard]] bool HasBundledProfile(const QString& title_id);
/// The folder a profile of the user's own goes in.
[[nodiscard]] QString UserProfileDirectory();
} // namespace ReaderBridge

} // namespace ReaderUi
