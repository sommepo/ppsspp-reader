// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>
#include "reader/save_sync/engine.h"
#include "reader/save_sync/host.h"

namespace SaveSync::Host {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct Shared {
    fs::path savedata;
    fs::path data;
    std::function<void(const std::string&)> say;

    Engine engine;

    std::mutex mutex;
    std::condition_variable idle;
    Settings settings;
    std::map<std::string, std::string> names; ///< by disc ID
    std::string running;                      ///< the disc ID of the game that runs
    std::string status;
    std::vector<Conflict> conflicts;
    int busy{};
    std::atomic<std::uint64_t> commits{0};
    std::atomic<std::uint64_t> generation{1};
};

// Kept by every background task, so that one that outlives Shutdown still has its ground.
std::shared_ptr<Shared> g_shared;
std::mutex g_shared_mutex;

std::shared_ptr<Shared> Current() {
    std::scoped_lock lock{g_shared_mutex};
    return g_shared;
}

Game GameFor(const Shared& shared, const std::string& disc, const std::string& name) {
    Game game;
    game.title = TitleOf(disc);
    game.name = name;
    const fs::path savedata = shared.savedata;
    game.locate = [savedata, disc] {
        std::vector<Folder> folders;
        std::error_code error;
        for (fs::directory_iterator walk{savedata, error}, end; !error && walk != end;
             walk.increment(error)) {
            const std::string folder = Utf8(walk->path().filename());
            if (walk->is_directory(error) && IsSaveFolderOf(disc, folder)) {
                folders.push_back({folder, walk->path()});
            }
        }
        std::sort(folders.begin(), folders.end(),
                  [](const Folder& a, const Folder& b) { return a.label < b.label; });
        return folders;
    };
    game.place = [savedata, disc](const std::string& label) -> std::optional<fs::path> {
        if (!IsSaveFolderOf(disc, label)) {
            return std::nullopt;
        }
        return savedata / FromUtf8(label);
    };
    return game;
}

std::string NameOf(Shared& shared, const std::string& disc) {
    std::scoped_lock lock{shared.mutex};
    const auto found = shared.names.find(disc);
    return found == shared.names.end() ? std::string{} : found->second;
}

void SaveSettings(Shared& shared) {
    Json names = Json::object();
    Json json;
    {
        std::scoped_lock lock{shared.mutex};
        for (const auto& [disc, name] : shared.names) {
            names[disc] = name;
        }
        json = {{"enabled", shared.settings.enabled},
                {"addresses", shared.settings.addresses},
                {"code", shared.settings.code},
                {"device", shared.settings.device},
                {"names", names}};
    }
    std::error_code error;
    fs::create_directories(shared.data, error);
    const fs::path file = shared.data / "settings.json";
    fs::path fresh = file;
    fresh += ".new";
    {
        std::ofstream out{fresh, std::ios::binary | std::ios::trunc};
        out << json.dump(1);
        if (!out) {
            return;
        }
    }
    fs::rename(fresh, file, error);
}

void LoadSettings(Shared& shared, const std::string& device_name) {
    shared.settings.device = device_name;
    std::ifstream in{shared.data / "settings.json", std::ios::binary};
    if (!in) {
        return;
    }
    std::stringstream text;
    text << in.rdbuf();
    const Json json = Json::parse(text.str(), nullptr, false);
    if (!json.is_object()) {
        return;
    }
    shared.settings.enabled = json.value("enabled", false);
    shared.settings.addresses = json.value("addresses", std::string{});
    shared.settings.code = json.value("code", std::string{});
    shared.settings.device = json.value("device", device_name);
    if (json.contains("names") && json["names"].is_object()) {
        for (const auto& [disc, name] : json["names"].items()) {
            if (name.is_string()) {
                shared.names[disc] = name.get<std::string>();
            }
        }
    }
}

void Configure(Shared& shared) {
    Config config;
    {
        std::scoped_lock lock{shared.mutex};
        config.enabled = shared.settings.enabled;
        config.addresses = shared.settings.addresses;
        config.code = shared.settings.code;
        config.device = shared.settings.device;
    }
    config.state_file = shared.data / "state.json";
    config.backup_dir = shared.data / "backups";
    shared.engine.Configure(config);
}

void SetStatus(Shared& shared, const std::string& text) {
    {
        std::scoped_lock lock{shared.mutex};
        shared.status = text;
    }
    ++shared.generation;
}

void DropConflict(Shared& shared, const std::string& title) {
    {
        std::scoped_lock lock{shared.mutex};
        std::erase_if(shared.conflicts, [&](const Conflict& c) { return c.title == title; });
    }
    ++shared.generation;
}

/// What a result means for the user: a word on screen, the status line, the open conflicts.
void Take(Shared& shared, const Result& result, bool background = false) {
    const bool conflict =
        result.kind == Result::Kind::Conflict || result.kind == Result::Kind::Later;
    if (conflict && !result.title.empty()) {
        Conflict entry;
        entry.title = result.title;
        entry.disc = DiscOf(result.title);
        entry.name = result.name.empty() ? NameOf(shared, entry.disc) : result.name;
        entry.message = "This device: saved " + WhenText(result.ours.newest_ms) + ". " +
                        (result.theirs.device.empty() ? std::string{"Another device"}
                                                      : result.theirs.device) +
                        ": saved " +
                        WhenText(result.theirs.saved_ms > 0 ? result.theirs.saved_ms
                                                            : result.theirs.time_ms) +
                        ".";
        {
            std::scoped_lock lock{shared.mutex};
            std::erase_if(shared.conflicts,
                          [&](const Conflict& c) { return c.title == entry.title; });
            shared.conflicts.push_back(entry);
        }
        ++shared.generation;
        const std::string name = entry.name.empty() ? entry.disc : entry.name;
        const std::string text = "Save sync: " + name +
                                 " has two different saves. This device's is used until you "
                                 "choose one under Save sync in the main menu.";
        SetStatus(shared, text);
        if (shared.say) {
            shared.say(text);
        }
        return;
    }
    if (!result.title.empty() &&
        (result.kind == Result::Kind::Sent || result.kind == Result::Kind::Fetched ||
         result.kind == Result::Kind::InSync)) {
        DropConflict(shared, result.title);
    }
    if (!result.message.empty()) {
        // What the background did is about a game the user is not looking at: named.
        std::string text = result.message;
        if (background && !result.title.empty()) {
            const std::string disc = DiscOf(result.title);
            const std::string name = result.name.empty() ? NameOf(shared, disc) : result.name;
            text += " (" + (name.empty() ? disc : name) + ")";
        }
        SetStatus(shared, text);
        if (shared.say) {
            shared.say(text);
        }
    }
}

/// Runs something beside the caller, which never waits for the network.
template <typename Work>
void Background(const std::shared_ptr<Shared>& shared, Work work) {
    {
        std::scoped_lock lock{shared->mutex};
        ++shared->busy;
    }
    std::thread([shared, work = std::move(work)]() mutable {
        try {
            work(*shared);
        } catch (const std::exception& error) {
            SetStatus(*shared, std::string{"Save sync: "} + error.what());
        } catch (...) {
            SetStatus(*shared, "Save sync: something went wrong.");
        }
        {
            std::scoped_lock lock{shared->mutex};
            --shared->busy;
        }
        shared->idle.notify_all();
    }).detach();
}

std::vector<Game> GamesHere(Shared& shared) {
    std::vector<Game> games;
    for (const std::string& disc : DiscsWithSaves(Utf8(shared.savedata))) {
        games.push_back(GameFor(shared, disc, NameOf(shared, disc)));
    }
    return games;
}

} // namespace

std::string TitleOf(std::string_view disc) {
    if (disc.size() != 9) {
        return {};
    }
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string title;
    for (std::size_t i = 0; i < 4; ++i) {
        const char letter = disc[i];
        if (letter < 'A' || letter > 'Z') {
            return {};
        }
        title.push_back(digits[(letter >> 4) & 15]);
        title.push_back(digits[letter & 15]);
    }
    title += "000";
    for (std::size_t i = 4; i < 9; ++i) {
        if (disc[i] < '0' || disc[i] > '9') {
            return {};
        }
        title.push_back(disc[i]);
    }
    return title;
}

std::string DiscOf(std::string_view title) {
    if (title.size() != 16 || title.substr(8, 3) != "000") {
        return {};
    }
    const auto value = [](char digit) {
        return digit >= '0' && digit <= '9' ? digit - '0'
               : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
    };
    std::string disc;
    for (std::size_t i = 0; i < 8; i += 2) {
        const int high = value(title[i]);
        const int low = value(title[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        disc.push_back(static_cast<char>(high * 16 + low));
    }
    disc += title.substr(11);
    return TitleOf(disc) == title ? disc : std::string{};
}

bool IsSaveFolderOf(std::string_view disc, std::string_view folder) {
    if (TitleOf(disc).empty() || folder.size() > 64 || !folder.starts_with(disc)) {
        return false;
    }
    return std::all_of(folder.begin(), folder.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

std::vector<std::string> DiscsWithSaves(const std::string& savedata) {
    std::vector<std::string> discs;
    std::error_code error;
    for (fs::directory_iterator walk{FromUtf8(savedata), error}, end; !error && walk != end;
         walk.increment(error)) {
        const std::string folder = Utf8(walk->path().filename());
        if (folder.size() < 9 || !walk->is_directory(error)) {
            continue;
        }
        const std::string disc = folder.substr(0, 9);
        if (IsSaveFolderOf(disc, folder) &&
            std::find(discs.begin(), discs.end(), disc) == discs.end()) {
            discs.push_back(disc);
        }
    }
    std::sort(discs.begin(), discs.end());
    return discs;
}

void Start(const std::string& savedata, const std::string& data,
           std::function<void(const std::string&)> say, const std::string& device_name) {
    try {
        auto shared = std::make_shared<Shared>();
        shared->savedata = FromUtf8(savedata);
        shared->data = FromUtf8(data);
        shared->say = std::move(say);
        LoadSettings(*shared, device_name);
        std::weak_ptr<Shared> weak = shared;
        shared->engine.SetListener([weak](const Result& result) {
            if (const auto alive = weak.lock()) {
                Take(*alive, result, !result.during_play);
            }
        });
        Configure(*shared);
        {
            std::scoped_lock lock{g_shared_mutex};
            g_shared = shared;
        }
        SyncAll();
    } catch (...) {
    }
}

bool Started() {
    return Current() != nullptr;
}

void Shutdown() {
    std::shared_ptr<Shared> shared;
    {
        std::scoped_lock lock{g_shared_mutex};
        shared = std::move(g_shared);
        g_shared.reset();
    }
    if (!shared) {
        return;
    }
    try {
        shared->engine.Unwatch();
        shared->engine.Flush(std::chrono::seconds(5));
        std::unique_lock lock{shared->mutex};
        shared->idle.wait_for(lock, std::chrono::seconds(3), [&] { return shared->busy == 0; });
    } catch (...) {
    }
}

Settings Get() {
    const auto shared = Current();
    if (!shared) {
        return {};
    }
    std::scoped_lock lock{shared->mutex};
    return shared->settings;
}

void Set(const Settings& settings) {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    try {
        {
            std::scoped_lock lock{shared->mutex};
            shared->settings = settings;
        }
        SaveSettings(*shared);
        Configure(*shared);
        SetStatus(*shared, {});
    } catch (...) {
    }
}

void GameStarting(const std::string& disc, const std::string& name) {
    const auto shared = Current();
    if (!shared || TitleOf(disc).empty()) {
        return;
    }
    try {
        bool fresh_name = false;
        {
            std::scoped_lock lock{shared->mutex};
            if (shared->running == disc) {
                return; // the game started another of its own programs
            }
            shared->running = disc;
            if (!name.empty() && shared->names[disc] != name) {
                shared->names[disc] = name;
                fresh_name = true;
            }
        }
        if (fresh_name) {
            SaveSettings(*shared);
        }
        if (!shared->engine.IsOn()) {
            return;
        }
        const Game game = GameFor(*shared, disc, name);
        const Result result = shared->engine.Prepare(game);
        if (result.kind == Result::Kind::Conflict) {
            shared->engine.Skip(game.title);
        }
        Take(*shared, result);
        Shared* const raw = shared.get();
        shared->engine.Watch(game, [raw] { return raw->commits.load(); });
    } catch (...) {
    }
}

void GameStopped() {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    try {
        {
            std::scoped_lock lock{shared->mutex};
            if (shared->running.empty()) {
                return;
            }
            shared->running.clear();
        }
        shared->engine.Unwatch();
    } catch (...) {
    }
}

void NoteSaved() {
    if (const auto shared = Current()) {
        ++shared->commits;
    }
}

std::string RunningDisc() {
    const auto shared = Current();
    if (!shared) {
        return {};
    }
    std::scoped_lock lock{shared->mutex};
    return shared->running;
}

void Check() {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    SetStatus(*shared, "Asking the service…");
    Background(shared, [](Shared& s) {
        if (!s.engine.IsOn()) {
            SetStatus(s, "Save sync is off, or the address or the code is missing.");
            return;
        }
        const Result result = s.engine.Check();
        SetStatus(s, result.message.empty() ? std::string{"The service answered."}
                                            : result.message);
    });
}

void SyncAll() {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    try {
        if (!shared->engine.IsOn()) {
            return;
        }
        shared->engine.SyncAll(GamesHere(*shared));
    } catch (...) {
    }
}

void Resolve(const std::string& title, bool keep_mine) {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    const std::string disc = DiscOf(title);
    if (disc.empty()) {
        return;
    }
    if (!keep_mine && RunningDisc() == disc) {
        SetStatus(*shared, "Stop the game first: its save cannot be replaced while it runs.");
        return;
    }
    SetStatus(*shared, "Working…");
    Background(shared, [disc, title, keep_mine](Shared& s) {
        const Game game = GameFor(s, disc, NameOf(s, disc));
        const Result result = s.engine.Resolve(game, keep_mine);
        if (result.kind == Result::Kind::Sent || result.kind == Result::Kind::Fetched ||
            result.kind == Result::Kind::InSync) {
            DropConflict(s, title);
            SetStatus(s, result.message.empty()
                             ? std::string{keep_mine ? "This device's save is the current one "
                                                       "now."
                                                     : "The other device's save is here now."}
                             : result.message);
        } else {
            SetStatus(s, result.message.empty() ? std::string{"That did not work."}
                                                : result.message);
        }
    });
}

void SendStats(const std::string& install, const std::string& json) {
    const auto shared = Current();
    if (!shared) {
        return;
    }
    try {
        if (shared->engine.IsOn()) {
            shared->engine.SendStats(install, json);
        }
    } catch (...) {
    }
}

std::string Status() {
    const auto shared = Current();
    if (!shared) {
        return {};
    }
    std::scoped_lock lock{shared->mutex};
    return shared->status;
}

std::vector<Conflict> Conflicts() {
    const auto shared = Current();
    if (!shared) {
        return {};
    }
    std::scoped_lock lock{shared->mutex};
    return shared->conflicts;
}

std::uint64_t Generation() {
    const auto shared = Current();
    return shared ? shared->generation.load() : 0;
}

bool Wait(int milliseconds) {
    const auto shared = Current();
    if (!shared) {
        return true;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    {
        std::unique_lock lock{shared->mutex};
        if (!shared->idle.wait_until(lock, until, [&] { return shared->busy == 0; })) {
            return false;
        }
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        until - std::chrono::steady_clock::now());
    return shared->engine.Flush(std::max(left, std::chrono::milliseconds(1)));
}

} // namespace SaveSync::Host
