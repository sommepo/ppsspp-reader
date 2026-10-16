// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include "reader/save_sync/engine.h"

namespace SaveSync {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

/// A game makes a save final in several steps at times: the save is sent once it has been
/// quiet for this long.
constexpr auto QuietTime = std::chrono::milliseconds(1500);
/// How long until a save that could not be sent is tried again.
constexpr auto RetryTime = std::chrono::seconds(30);
constexpr auto UnsentRetryTime = std::chrono::seconds(60);
/// A save that could not be read (a file in use) is looked at again this soon, this often.
constexpr auto ReadRetryTime = std::chrono::seconds(3);
constexpr int ReadAttempts = 5;
/// Replaced saves kept on this device, for each game.
constexpr std::size_t BackupsKept = 10;

std::tm LocalTime(std::int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    return parts;
}

std::string Formatted(std::int64_t ms, const char* format) {
    const std::tm parts = LocalTime(ms);
    char buffer[40];
    const std::size_t length = std::strftime(buffer, sizeof(buffer), format, &parts);
    return {buffer, length};
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// When a version's save was made: by its device's clock where that is known.
std::int64_t MadeMs(const Version& version) {
    return version.saved_ms > 0 ? version.saved_ms : version.time_ms;
}

std::string DeviceText(const Version& version) {
    return version.device.empty() ? std::string{"another device"} : version.device;
}

std::string NameText(const Game& game) {
    return game.name.empty() ? game.title : game.name;
}

/// Where the game's save is now.
std::vector<Folder> Here(const Game& game) {
    return game.locate ? game.locate() : game.folders;
}

/// Where the save of an archive goes: the folders here, and a place for each one that only
/// the archive has.
std::vector<Folder> HereFor(const Game& game, std::string_view zip) {
    std::vector<Folder> folders = Here(game);
    if (!game.place) {
        return folders;
    }
    for (const std::string& label : LabelsOfArchive(zip)) {
        const bool known = std::any_of(folders.begin(), folders.end(),
                                       [&](const Folder& f) { return f.label == label; });
        if (known) {
            continue;
        }
        if (const auto path = game.place(label)) {
            folders.push_back({label, *path});
        }
    }
    return folders;
}

Result Make(Result::Kind kind, const Game& game, std::string message) {
    Result result;
    result.kind = kind;
    result.title = game.title;
    result.name = game.name;
    result.message = std::move(message);
    return result;
}

Result FromError(const RemoteError& error, const Game& game) {
    switch (error.failure) {
    case Failure::Offline:
        return Make(Result::Kind::Offline, game,
                    "Save sync: the service cannot be reached. This device's save is used, and "
                    "sent when the service is there again.");
    case Failure::Unauthorised:
        return Make(Result::Kind::Failed, game, "Save sync: the code is not the service's.");
    default:
        return Make(Result::Kind::Failed, game, "Save sync: " + error.message + ".");
    }
}

} // namespace

std::string WhenText(std::int64_t ms) {
    return ms > 0 ? Formatted(ms, "%Y-%m-%d %H:%M") : std::string{"at an unknown time"};
}

Step Decide(const State& local, const std::optional<Record>& record, const TitleInfo& remote) {
    if (!remote.head) {
        return local.Empty() ? Step::Nothing : Step::Upload;
    }
    const Version& head = *remote.head;
    if (local.Empty()) {
        return Step::Download;
    }
    if (local.hash == head.hash) {
        return Step::Nothing;
    }
    const bool unchanged_here = record && record->hash == local.hash;
    if (unchanged_here && record->base != head.n) {
        return Step::Download;
    }
    if (!unchanged_here && record && record->base == head.n) {
        return Step::Upload;
    }
    // A save the service has as an older version: nothing is lost by moving on from it.
    const bool known = std::any_of(remote.versions.begin(), remote.versions.end(),
                                   [&](const Version& v) { return v.hash == local.hash; });
    return known ? Step::Download : Step::Ask;
}

Engine::Engine() : thread{[this] { Run(); }} {}

Engine::~Engine() {
    {
        std::scoped_lock lock{mutex};
        stopping = true;
    }
    wake.notify_all();
    thread.join();
}

void Engine::Configure(const Config& config_) {
    std::scoped_lock lock{mutex};
    if (configured && config.enabled == config_.enabled &&
        config.addresses == config_.addresses && config.code == config_.code &&
        config.device == config_.device && config.state_file == config_.state_file &&
        config.backup_dir == config_.backup_dir) {
        return;
    }
    configured = true;
    config = config_;
    remote.reset();
    if (config.enabled && !config.code.empty()) {
        auto addresses = ParseAddresses(config.addresses);
        if (!addresses.empty()) {
            remote = std::make_shared<Remote>(std::move(addresses), config.code, config.device);
        }
    }
    suspended.clear();
    offline_reported = false;
    LoadRecords();
}

bool Engine::IsOn() const {
    std::scoped_lock lock{mutex};
    return remote != nullptr;
}

void Engine::SetListener(std::function<void(const Result&)> listener_) {
    std::scoped_lock lock{mutex};
    listener = std::move(listener_);
}

Engine::Setup Engine::CurrentSetup() const {
    std::scoped_lock lock{mutex};
    return {config, remote};
}

std::optional<Record> Engine::RecordOf(const std::string& title) const {
    std::scoped_lock lock{mutex};
    const auto found = records.find(title);
    return found == records.end() ? std::nullopt : std::optional<Record>{found->second};
}

void Engine::SetRecord(const std::string& title, const Record& record) {
    std::scoped_lock lock{mutex};
    const auto found = records.find(title);
    if (found != records.end() && found->second.base == record.base &&
        found->second.hash == record.hash) {
        return;
    }
    records[title] = record;
    SaveRecords();
}

RemoteError Engine::Describe(const Setup& setup, const std::string& title, TitleInfo& out,
                             const std::string& name) {
    if (const RemoteError error = setup.remote->Describe(title, out, name)) {
        return error;
    }
    std::scoped_lock lock{mutex};
    if (!out.service_id.empty() && out.service_id != service_id) {
        if (!service_id.empty()) {
            // Without a note a save that differs from the service's is asked about, which is
            // right for a service this device has not synced with.
            records.clear();
        }
        service_id = out.service_id;
        SaveRecords();
    }
    return {};
}

void Engine::LoadRecords() {
    records.clear();
    service_id.clear();
    if (config.state_file.empty()) {
        return;
    }
    std::ifstream in{config.state_file, std::ios::binary};
    if (!in) {
        return;
    }
    const Json json = Json::parse(in, nullptr, false);
    if (!json.is_object() || !json.contains("titles") || !json["titles"].is_object()) {
        return;
    }
    if (json.contains("service") && json["service"].is_string()) {
        service_id = json["service"].get<std::string>();
    }
    for (const auto& [title, entry] : json["titles"].items()) {
        if (entry.is_object()) {
            records[title] = {entry.value("base", 0), entry.value("hash", std::string{})};
        }
    }
}

void Engine::SaveRecords() const {
    if (config.state_file.empty()) {
        return;
    }
    Json titles = Json::object();
    for (const auto& [title, record] : records) {
        titles[title] = {{"base", record.base}, {"hash", record.hash}};
    }
    const Json json = {{"service", service_id}, {"titles", titles}};
    std::error_code error;
    std::filesystem::create_directories(config.state_file.parent_path(), error);
    std::filesystem::path temporary = config.state_file;
    temporary += ".new";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out << json.dump(1);
        out.flush();
        if (!out) {
            return;
        }
    }
    std::filesystem::rename(temporary, config.state_file, error);
}

void Engine::Emit(const Result& result) const {
    std::function<void(const Result&)> to;
    {
        std::scoped_lock lock{mutex};
        to = listener;
    }
    if (to) {
        to(result);
    }
}

Result Engine::Check() {
    const Setup setup = CurrentSetup();
    const Game none;
    if (!setup.remote) {
        return Make(Result::Kind::Off, none,
                    setup.config.enabled
                        ? "Save sync needs the service's address and its code."
                        : "Save sync is switched off.");
    }
    if (const RemoteError error = setup.remote->Ping()) {
        Result result = FromError(error, none);
        if (error.failure == Failure::Offline) {
            result.message = "Save sync: " + error.message + ".";
        }
        return result;
    }
    std::vector<TitleInfo> saves;
    if (const RemoteError error = setup.remote->List(saves)) {
        return FromError(error, none);
    }
    return Make(Result::Kind::InSync, none,
                "Connected to " + setup.remote->LastAddress() + ". The service has saves of " +
                    std::to_string(saves.size()) + (saves.size() == 1 ? " game." : " games."));
}

Result Engine::Fetch(const Setup& setup, const Game& game, const Version& head) {
    std::string zip;
    if (const RemoteError error = setup.remote->Download(game.title, head.n, zip)) {
        return FromError(error, game);
    }
    try {
        if (StateOfArchive(zip).hash != head.hash) {
            return Make(Result::Kind::Failed, game,
                        "Save sync: the save arrived damaged. This device's save was left as "
                        "it is.");
        }
        const std::filesystem::path backups = setup.config.backup_dir / game.title;
        std::filesystem::path backup = backups / Formatted(NowMs(), "%Y%m%d-%H%M%S");
        for (int again = 2; std::filesystem::exists(backup); ++again) {
            backup = backups / (Formatted(NowMs(), "%Y%m%d-%H%M%S") + "-" + std::to_string(again));
        }
        const std::vector<Folder> placed = HereFor(game, zip);
        Unpack(zip, placed, backup);
        if (game.unpacked) {
            game.unpacked(placed);
        }
        PruneBackups(backups, BackupsKept);
    } catch (const ArchiveError& error) {
        return Make(Result::Kind::Failed, game, std::string{"Save sync: "} + error.what() + ".");
    }
    SetRecord(game.title, {head.n, head.hash});
    Result result = Make(Result::Kind::Fetched, game,
                         "Save sync: took the save made on " + DeviceText(head) + " (" +
                             WhenText(MadeMs(head)) + ").");
    result.theirs = head;
    return result;
}

Result Engine::Send(const Setup& setup, const Game& game, const Snapshot& snapshot,
                    UploadMode mode, bool ask) {
    const std::optional<Record> record = RecordOf(game.title);
    int version = 0;
    std::optional<Version> head;
    const RemoteError error = setup.remote->Upload(game.title, game.name, snapshot,
                                                   record ? record->base : 0, mode, version, head);
    if (error.failure == Failure::Conflict) {
        Result result = Make(ask ? Result::Kind::Conflict : Result::Kind::Later, game, {});
        result.ours = snapshot.state;
        if (head) {
            result.theirs = *head;
        }
        result.message = "Save sync: " + DeviceText(result.theirs) + " saved " + NameText(game) +
                         " in the meantime (" + WhenText(MadeMs(result.theirs)) +
                         "). Nothing is sent; you are asked which save to keep when the game is "
                         "next started.";
        return result;
    }
    if (error) {
        return FromError(error, game);
    }
    if (mode != UploadMode::Side) {
        SetRecord(game.title, {version, snapshot.state.hash});
    }
    return Make(Result::Kind::Sent, game, "Save sync: this device's save was sent.");
}

Result Engine::SyncOne(const Setup& setup, const Game& game, bool ask) {
    try {
        const State local = StateOf(Here(game));
        TitleInfo info;
        if (const RemoteError error = Describe(setup, game.title, info, game.name)) {
            return FromError(error, game);
        }
        switch (Decide(local, RecordOf(game.title), info)) {
        case Step::Nothing:
            if (info.head && !local.Empty()) {
                SetRecord(game.title, {info.head->n, info.head->hash});
            }
            return Make(Result::Kind::InSync, game, {});
        case Step::Upload:
            return Send(setup, game, Pack(Here(game)), UploadMode::Normal, ask);
        case Step::Download:
            return Fetch(setup, game, *info.head);
        case Step::Ask:
            break;
        }
        Result result = Make(ask ? Result::Kind::Conflict : Result::Kind::Later, game, {});
        result.ours = local;
        result.theirs = *info.head;
        result.message = "Save sync: " + NameText(game) + " has one save here (" +
                         WhenText(local.newest_ms) + ") and another from " +
                         DeviceText(*info.head) + " (" + WhenText(MadeMs(*info.head)) +
                         "). You are asked which to keep when the game is started.";
        return result;
    } catch (const ArchiveError& error) {
        return Make(Result::Kind::Failed, game, std::string{"Save sync: "} + error.what() + ".");
    }
}

Result Engine::Prepare(const Game& game) {
    const Setup setup = CurrentSetup();
    if (!setup.remote) {
        return Make(Result::Kind::Off, game, {});
    }
    {
        std::scoped_lock lock{mutex};
        active = game.title;
        suspended.erase(game.title);
        unsent.erase(game.title);
    }
    std::scoped_lock busy{work};
    const Result result = SyncOne(setup, game, true);
    {
        // Said here already: not again at the first save made while the service is away.
        std::scoped_lock lock{mutex};
        offline_reported = result.kind == Result::Kind::Offline;
    }
    return result;
}

Result Engine::Resolve(const Game& game, bool keep_mine) {
    const Setup setup = CurrentSetup();
    if (!setup.remote) {
        return Make(Result::Kind::Off, game, {});
    }
    {
        std::scoped_lock lock{mutex};
        suspended.erase(game.title);
    }
    std::scoped_lock busy{work};
    try {
        const Snapshot snapshot = Pack(Here(game));
        if (keep_mine) {
            if (snapshot.state.Empty()) {
                return Make(Result::Kind::InSync, game, {});
            }
            return Send(setup, game, snapshot, UploadMode::Force, true);
        }
        if (!snapshot.state.Empty()) {
            // The save given up here stays on the service, beside the versions.
            const Result kept = Send(setup, game, snapshot, UploadMode::Side, true);
            if (kept.kind != Result::Kind::Sent) {
                return kept;
            }
        }
        TitleInfo info;
        if (const RemoteError error = Describe(setup, game.title, info)) {
            return FromError(error, game);
        }
        if (!info.head) {
            return Make(Result::Kind::Failed, game,
                        "Save sync: the service has no save of this game any more.");
        }
        return Fetch(setup, game, *info.head);
    } catch (const ArchiveError& error) {
        return Make(Result::Kind::Failed, game, std::string{"Save sync: "} + error.what() + ".");
    }
}

void Engine::Skip(const std::string& title) {
    std::scoped_lock lock{mutex};
    suspended.insert(title);
}

void Engine::Release() {
    std::scoped_lock lock{mutex};
    if (!watching) {
        active.clear();
    }
}

void Engine::Watch(const Game& game, std::function<std::uint64_t()> commits_) {
    std::scoped_lock lock{mutex};
    watching = game;
    commits = std::move(commits_);
    seen = commits ? commits() : 0;
    dirty = false;
    retry_at = {};
    active = game.title;
}

void Engine::Unwatch() {
    {
        std::scoped_lock lock{mutex};
        active.clear();
        if (!watching) {
            return;
        }
        const Game game = *watching;
        watching.reset();
        commits = nullptr;
        dirty = false;
        jobs.push_back([this, game] {
            if (Push(game, false) != Pushed::Done) {
                std::scoped_lock again{mutex};
                unsent[game.title] = game;
                unsent_retry_at = Clock::now() + UnsentRetryTime;
            }
        });
    }
    wake.notify_all();
}

void Engine::SendStats(std::string install, std::string json) {
    {
        std::scoped_lock lock{mutex};
        if (!remote) {
            return;
        }
        jobs.push_back([this, install = std::move(install), json = std::move(json)] {
            const Setup setup = CurrentSetup();
            if (setup.remote) {
                (void)setup.remote->PutStats(install, json);
            }
        });
    }
    wake.notify_all();
}

void Engine::SyncAll(std::vector<Game> games) {
    {
        std::scoped_lock lock{mutex};
        if (!remote) {
            return;
        }
        jobs.push_back([this, games = std::move(games)] {
            for (const Game& game : games) {
                const Setup setup = CurrentSetup();
                if (!setup.remote) {
                    return;
                }
                {
                    std::scoped_lock lock{mutex};
                    if (stopping) {
                        return;
                    }
                    if (game.title == active || suspended.contains(game.title)) {
                        continue;
                    }
                }
                Result result;
                {
                    std::scoped_lock busy{work};
                    // The game may have been started while this one waited.
                    {
                        std::scoped_lock lock{mutex};
                        if (game.title == active) {
                            continue;
                        }
                    }
                    result = SyncOne(setup, game, false);
                }
                if (result.kind != Result::Kind::InSync) {
                    Emit(result);
                }
                if (result.kind == Result::Kind::Offline) {
                    return;
                }
            }
        });
    }
    wake.notify_all();
}

Engine::Pushed Engine::Push(const Game& game, bool during_play) {
    const Setup setup = CurrentSetup();
    if (!setup.remote) {
        return Pushed::Done;
    }
    {
        std::scoped_lock lock{mutex};
        if (suspended.contains(game.title)) {
            return Pushed::Done;
        }
    }
    Result result;
    {
        std::scoped_lock busy{work};
        try {
            const Snapshot snapshot = Pack(Here(game));
            {
                std::scoped_lock lock{mutex};
                read_failures = 0;
            }
            const std::optional<Record> record = RecordOf(game.title);
            if (snapshot.state.Empty() || (record && record->hash == snapshot.state.hash)) {
                return Pushed::Done;
            }
            result = Send(setup, game, snapshot, UploadMode::Normal, false);
        } catch (const ArchiveError& error) {
            // A file the game is writing this moment cannot be read on every system: looked
            // at again a few times before it is called a failure.
            std::scoped_lock lock{mutex};
            if (++read_failures < ReadAttempts) {
                return Pushed::Soon;
            }
            read_failures = 0;
            result = Make(Result::Kind::Failed, game,
                          std::string{"Save sync: "} + error.what() + ".");
        }
    }
    result.during_play = during_play;
    bool report = true;
    {
        std::scoped_lock lock{mutex};
        switch (result.kind) {
        case Result::Kind::Sent:
            offline_reported = false;
            break;
        case Result::Kind::Offline:
            // Said once, not at every save made while the service is away.
            report = !offline_reported;
            offline_reported = true;
            break;
        default:
            // A conflict, or a save the service does not take: not tried over and over.
            suspended.insert(game.title);
            break;
        }
    }
    if (report) {
        Emit(result);
    }
    return result.kind == Result::Kind::Offline ? Pushed::Later : Pushed::Done;
}

void Engine::Run() {
    std::unique_lock lock{mutex};
    while (!stopping) {
        if (jobs.empty() && !dirty) {
            idle.notify_all();
        }
        wake.wait_for(lock, std::chrono::milliseconds(250));
        if (stopping) {
            break;
        }
        if (!jobs.empty()) {
            const auto job = std::move(jobs.front());
            jobs.pop_front();
            working = true;
            lock.unlock();
            job();
            lock.lock();
            working = false;
            continue;
        }
        const auto now = Clock::now();
        if (watching && commits) {
            const std::uint64_t count = commits();
            if (count != seen) {
                seen = count;
                dirty = true;
                changed_at = now;
                // A new save is worth a new try, also right after one that failed.
                retry_at = {};
            }
            if (dirty && now - changed_at >= QuietTime && now >= retry_at) {
                const Game game = *watching;
                const std::uint64_t sent_at = seen;
                working = true;
                lock.unlock();
                const Pushed pushed = Push(game, true);
                lock.lock();
                working = false;
                if (pushed == Pushed::Later) {
                    retry_at = Clock::now() + RetryTime;
                } else if (pushed == Pushed::Soon) {
                    retry_at = Clock::now() + ReadRetryTime;
                } else if (!commits || commits() == sent_at) {
                    dirty = false;
                }
                continue;
            }
        }
        if (!unsent.empty() && now >= unsent_retry_at) {
            unsent_retry_at = now + UnsentRetryTime;
            std::vector<Game> again;
            for (const auto& [title, game] : unsent) {
                if (title != active) {
                    again.push_back(game);
                }
            }
            working = true;
            lock.unlock();
            std::vector<std::string> sent;
            for (const Game& game : again) {
                if (Push(game, false) != Pushed::Done) {
                    break;
                }
                sent.push_back(game.title);
            }
            lock.lock();
            working = false;
            for (const std::string& title : sent) {
                unsent.erase(title);
            }
        }
    }
    idle.notify_all();
}

bool Engine::Flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    // A save made final this moment counts, though the engine has not looked yet.
    if (watching && commits && commits() != seen) {
        seen = commits();
        dirty = true;
        changed_at = Clock::now();
    }
    wake.notify_all();
    return idle.wait_for(lock, timeout,
                         [this] { return stopping || (jobs.empty() && !working && !dirty); });
}

std::vector<OverviewRow> Engine::Overview(
    const std::function<std::vector<Folder>(const std::string& title)>& folders_of,
    std::string& error) {
    std::vector<OverviewRow> rows;
    const Setup setup = CurrentSetup();
    if (!setup.remote) {
        error = "Save sync is not set up.";
        return rows;
    }
    std::vector<TitleInfo> saves;
    if (const RemoteError failed = setup.remote->List(saves)) {
        error = FromError(failed, {}).message;
        return rows;
    }
    const auto state_here = [&](const std::string& title) -> std::optional<State> {
        try {
            const std::vector<Folder> folders = folders_of ? folders_of(title)
                                                           : std::vector<Folder>{};
            return folders.empty() ? std::nullopt : std::optional<State>{StateOf(folders)};
        } catch (const ArchiveError&) {
            return std::nullopt;
        }
    };
    std::set<std::string> listed;
    for (const TitleInfo& save : saves) {
        OverviewRow row;
        row.title = save.title;
        row.name = save.name;
        row.head = save.head;
        const std::optional<State> here = state_here(save.title);
        if (!here || here->Empty()) {
            row.here = "none here";
        } else {
            row.here = save.head && here->hash == save.head->hash ? "same" : "differs";
        }
        listed.insert(save.title);
        rows.push_back(std::move(row));
    }
    std::map<std::string, Record> noted;
    {
        std::scoped_lock lock{mutex};
        noted = records;
    }
    for (const auto& [title, record] : noted) {
        if (!listed.contains(title)) {
            rows.push_back({title, {}, std::nullopt, "only here"});
        }
    }
    return rows;
}

Result Engine::History(const std::string& title, TitleInfo& out) {
    const Setup setup = CurrentSetup();
    Game game;
    game.title = title;
    if (!setup.remote) {
        return Make(Result::Kind::Off, game, "Save sync is not set up.");
    }
    if (const RemoteError error = Describe(setup, title, out)) {
        return FromError(error, game);
    }
    return Make(Result::Kind::InSync, game, {});
}

Result Engine::Restore(const Game& game, int version) {
    const Setup setup = CurrentSetup();
    if (!setup.remote) {
        return Make(Result::Kind::Off, game, "Save sync is not set up.");
    }
    {
        std::scoped_lock lock{mutex};
        if (active == game.title) {
            return Make(Result::Kind::Failed, game,
                        "Stop the game first: its save cannot be replaced while it runs.");
        }
        suspended.erase(game.title);
        unsent.erase(game.title);
    }
    std::scoped_lock busy{work};
    try {
        const Snapshot snapshot = Pack(Here(game));
        if (!snapshot.state.Empty()) {
            const Result kept = Send(setup, game, snapshot, UploadMode::Side, true);
            if (kept.kind != Result::Kind::Sent) {
                return kept;
            }
        }
        int new_version = 0;
        if (const RemoteError error = setup.remote->Restore(game.title, version, new_version)) {
            return FromError(error, game);
        }
        TitleInfo info;
        if (const RemoteError error = Describe(setup, game.title, info)) {
            return FromError(error, game);
        }
        if (!info.head) {
            return Make(Result::Kind::Failed, game, "Save sync: the version is not there any more.");
        }
        return Fetch(setup, game, *info.head);
    } catch (const ArchiveError& error) {
        return Make(Result::Kind::Failed, game, std::string{"Save sync: "} + error.what() + ".");
    }
}

} // namespace SaveSync
