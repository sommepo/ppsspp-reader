// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "reader/save_sync/archive.h"
#include "reader/save_sync/remote.h"

namespace SaveSync {

struct Config {
    bool enabled{};
    /// The service's addresses as the user wrote them (see ParseAddresses).
    std::string addresses;
    std::string code;
    /// What this device is called in the other devices' messages.
    std::string device;
    /// Where this device notes which version each save came from.
    std::filesystem::path state_file;
    /// Where a save goes that another one took the place of.
    std::filesystem::path backup_dir;
};

/// A game and where its save is on this device.
struct Game {
    /// The game's ID, 16 hexadecimal digits in capitals (TitleText).
    std::string title;
    /// Its name, for the other devices' lists; may be empty.
    std::string name;
    std::vector<Folder> folders;
    /// A PSP game's save is as many folders as the game cares to make, named by the game.
    /// With `locate`, the folders are asked for each time instead of taken from `folders`,
    /// and `place` says where a folder of another device's save belongs here (nothing for a
    /// name that is not this game's).
    std::function<std::vector<Folder>()> locate;
    std::function<std::optional<std::filesystem::path>(const std::string& label)> place;
};

/// What this device notes about a save after syncing it: the service's version it is, and its
/// state then. A save whose state is still that one has not changed here since.
struct Record {
    int base{};
    std::string hash;
};

enum class Step {
    Nothing,  ///< The two are the same, or there is nothing on either side.
    Upload,   ///< This device's save is the newer one.
    Download, ///< The service's save is the newer one, or this device has none.
    Ask,      ///< Both changed: only the user knows which to keep.
};

/// What is to be done with a save, from its state here, this device's note about it and what
/// the service has.
[[nodiscard]] Step Decide(const State& local, const std::optional<Record>& record,
                          const TitleInfo& remote);

struct Result {
    enum class Kind {
        Off,      ///< Save sync is not switched on, or not set up.
        InSync,   ///< Nothing had to be done.
        Sent,     ///< This device's save went to the service.
        Fetched,  ///< The service's save took the place of this device's.
        Conflict, ///< Both changed: answer with Engine::Resolve or Engine::Skip.
        Later,    ///< Both changed, found while no question can be put: asked at the next start.
        Offline,  ///< The service did not answer; this device's save is used.
        Failed,
    };
    Kind kind{Kind::Off};
    std::string title;
    std::string name;
    /// A sentence for the user; empty when there is nothing to say.
    std::string message;
    /// Found while the game was running.
    bool during_play{};
    /// Conflict and Later: this device's save and the service's current version.
    State ours;
    Version theirs;
};

/// A game in the overview: what the service has, and how this device's save compares.
struct OverviewRow {
    std::string title;
    std::string name;
    std::optional<Version> head;
    /// "same", "differs", "none here" or "only here".
    std::string here;
};

/// A time as the messages show it (this device's time zone): 2026-10-04 21:14.
[[nodiscard]] std::string WhenText(std::int64_t ms);

/**
 * Keeps the saves of this device and the service's the same.
 *
 * Before a game starts, Prepare fetches the service's save when it is the newer one, sends
 * this device's when that is, and reports a conflict when both changed. While the game runs,
 * Watch sends the save a moment after each time the game made it final. Nothing is ever
 * replaced without being kept: on the service every version stays, and on this device a
 * replaced save is moved to the backup folder.
 *
 * Prepare, Resolve, Restore, Check and Overview block (the network) and may be called from
 * any thread; the rest returns at once. The listener is called on the engine's own thread.
 */
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Configure(const Config& config_);
    [[nodiscard]] bool IsOn() const;

    /// Hears of what happens in the background (Watch, SyncAll, a retry).
    void SetListener(std::function<void(const Result&)> listener_);

    /// Asks the service whether the address and the code are right.
    [[nodiscard]] Result Check();

    /// Before `game` starts. Its save is not touched by the background from here until
    /// Unwatch or Release.
    [[nodiscard]] Result Prepare(const Game& game);
    /// Answers a conflict: this device's save becomes the current one, or the service's takes
    /// its place here. The other one is kept on the service either way.
    [[nodiscard]] Result Resolve(const Game& game, bool keep_mine);
    /// Leaves a conflict open: nothing of this game is sent until it is next prepared.
    void Skip(const std::string& title);
    /// The game prepared did not start after all.
    void Release();

    /// While `game` runs: `commits` counts the times it made a save final.
    void Watch(const Game& game, std::function<std::uint64_t()> commits);
    /// The game stopped: its save is sent a last time, in the background.
    void Unwatch();

    /// In the background: brings each of the games in line where that needs no question.
    void SyncAll(std::vector<Game> games);
    /// In the background: sends this install's reading statistics (Remote::PutStats). Nothing
    /// is said when it fails: the record is whole each time, the next one makes up for it.
    void SendStats(std::string install, std::string json);
    /// Waits until the background has nothing left to do. False when the time ran out.
    bool Flush(std::chrono::milliseconds timeout);

    /// The games the service has and those this device has a note about. `folders_of` gives a
    /// game's save folders here (empty when the game is not known to the frontend).
    [[nodiscard]] std::vector<OverviewRow> Overview(
        const std::function<std::vector<Folder>(const std::string& title)>& folders_of,
        std::string& error);
    /// A game's versions on the service.
    [[nodiscard]] Result History(const std::string& title, TitleInfo& out);
    /// Makes an older version the current one, here and on the service. Not for a running game.
    [[nodiscard]] Result Restore(const Game& game, int version);

private:
    struct Setup {
        Config config;
        std::shared_ptr<Remote> remote;
    };

    void Run();
    [[nodiscard]] Setup CurrentSetup() const;
    [[nodiscard]] std::optional<Record> RecordOf(const std::string& title) const;
    void SetRecord(const std::string& title, const Record& record);
    void LoadRecords();
    void SaveRecords() const;
    /// Asks the service about a game, and drops the notes when it is another service than
    /// the one they were made with: its version numbers mean other saves.
    RemoteError Describe(const Setup& setup, const std::string& title, TitleInfo& out,
                         const std::string& name = {});
    void Emit(const Result& result) const;

    /// One game brought in line. `ask`: a conflict is returned as one (else as Later).
    Result SyncOne(const Setup& setup, const Game& game, bool ask);
    Result Fetch(const Setup& setup, const Game& game, const Version& head);
    Result Send(const Setup& setup, const Game& game, const Snapshot& snapshot, UploadMode mode,
                bool ask);
    enum class Pushed {
        Done,  ///< Sent, or there was nothing to send, or it is not to be tried again.
        Soon,  ///< The save could not be read this moment.
        Later, ///< The service did not answer.
    };
    /// Sends a running (or just stopped) game's save if it changed.
    Pushed Push(const Game& game, bool during_play);

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    Config config;
    bool configured{};
    std::shared_ptr<Remote> remote;
    std::map<std::string, Record> records;
    /// The service the notes are about; empty until one said what it is.
    std::string service_id;
    std::function<void(const Result&)> listener;

    /// The game prepared or running: the background leaves it alone.
    std::string active;
    /// Games with an open conflict: nothing is sent for them.
    std::set<std::string> suspended;

    std::optional<Game> watching;
    std::function<std::uint64_t()> commits;
    std::uint64_t seen{};
    bool dirty{};
    std::chrono::steady_clock::time_point changed_at;
    std::chrono::steady_clock::time_point retry_at;
    /// Saves that could not be sent after their game stopped; tried again now and then.
    std::map<std::string, Game> unsent;
    std::chrono::steady_clock::time_point unsent_retry_at;
    bool offline_reported{};
    int read_failures{};

    std::deque<std::function<void()>> jobs;
    bool working{};
    bool stopping{};
    /// One operation at a time touches saves and talks to the service.
    std::mutex work;
    std::thread thread;
};

} // namespace SaveSync
