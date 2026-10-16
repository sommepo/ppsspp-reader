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
#include <set>
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
    /// PSP/SAVEDATA, or with a store the copies of its folders.
    fs::path savedata;
    fs::path data;
    Store store;
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

std::string Join(const std::string& folder, const std::string& name) {
    return folder.empty() ? name : folder + "/" + name;
}

std::string ReadNative(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::stringstream content;
    content << in.rdbuf();
    if (!in) {
        throw ArchiveError("a copy of a save could not be read (" + Utf8(path) + ")");
    }
    return content.str();
}

void WriteNative(const fs::path& path, std::string_view content) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        throw ArchiveError("a copy of a save could not be written (" + Utf8(path) + ")");
    }
}

void SetWrittenMs(const fs::path& path, std::int64_t ms) {
    if (ms <= 0) {
        return;
    }
    // The file clock's start is not the same everywhere: go by the distance from now.
    const auto ago = std::chrono::system_clock::now() -
                     std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
    std::error_code error;
    fs::last_write_time(
        path,
        fs::file_time_type::clock::now() -
            std::chrono::duration_cast<fs::file_time_type::duration>(ago),
        error);
}

/// The folders of SAVEDATA in a store; throws when it cannot be read (an empty answer would
/// look like a game without a save, which a fetch may then fill).
std::vector<Store::Entry> StoreTop(const Store& store) {
    std::vector<Store::Entry> top;
    if (!store.list("", top)) {
        throw ArchiveError("the memory stick's SAVEDATA folder could not be read");
    }
    return top;
}

/// Makes `here` a copy of the store's folder `path`: what differs is written, what the store
/// no longer has is removed.
void CopyIn(const Store& store, const std::string& path, const fs::path& here) {
    std::vector<Store::Entry> entries;
    if (!store.list(path, entries)) {
        throw ArchiveError("a save folder on the memory stick could not be read (" + path + ")");
    }
    std::error_code error;
    if (!fs::is_directory(here, error)) {
        fs::remove(here, error);
        fs::create_directories(here, error);
        if (error) {
            throw ArchiveError("no copy of a save could be made (" + Utf8(here) + ")");
        }
    }
    std::set<std::string> kept;
    for (const Store::Entry& entry : entries) {
        const std::string below = Join(path, entry.name);
        const fs::path target = here / FromUtf8(entry.name);
        kept.insert(entry.name);
        if (entry.folder) {
            CopyIn(store, below, target);
            continue;
        }
        std::string content;
        if (!store.read(below, content)) {
            throw ArchiveError("a file of a save on the memory stick could not be read (" +
                               below + ")");
        }
        if (fs::is_directory(target, error)) {
            fs::remove_all(target, error);
        }
        if (!fs::is_regular_file(target, error) || ReadNative(target) != content) {
            WriteNative(target, content);
            SetWrittenMs(target, entry.modified_ms);
        }
    }
    std::vector<fs::path> gone;
    for (fs::directory_iterator walk{here, error}, end; !error && walk != end;
         walk.increment(error)) {
        if (!kept.contains(Utf8(walk->path().filename()))) {
            gone.push_back(walk->path());
        }
    }
    for (const fs::path& path_gone : gone) {
        fs::remove_all(path_gone, error);
    }
}

/// A game's folders of a store, as copies in `staging` brought up to date.
std::vector<Folder> CopyInGame(const Store& store, const fs::path& staging,
                               const std::string& disc) {
    std::vector<Folder> folders;
    for (const Store::Entry& entry : StoreTop(store)) {
        if (!entry.folder || !IsSaveFolderOf(disc, entry.name)) {
            continue;
        }
        const fs::path here = staging / FromUtf8(entry.name);
        CopyIn(store, entry.name, here);
        folders.push_back({entry.name, here});
    }
    // Copies of folders the memory stick no longer has.
    std::error_code error;
    std::vector<fs::path> gone;
    for (fs::directory_iterator walk{staging, error}, end; !error && walk != end;
         walk.increment(error)) {
        const std::string name = Utf8(walk->path().filename());
        const bool known = std::any_of(folders.begin(), folders.end(),
                                       [&](const Folder& f) { return f.label == name; });
        if (!known && IsSaveFolderOf(disc, name)) {
            gone.push_back(walk->path());
        }
    }
    for (const fs::path& path : gone) {
        fs::remove_all(path, error);
    }
    std::sort(folders.begin(), folders.end(),
              [](const Folder& a, const Folder& b) { return a.label < b.label; });
    return folders;
}

/// Writes the copy `here` into the store's folder `path`, which was just made.
void CopyOut(const Store& store, const fs::path& here, const std::string& path) {
    std::error_code error;
    std::vector<fs::path> entries;
    for (fs::directory_iterator walk{here, error}, end; !error && walk != end;
         walk.increment(error)) {
        entries.push_back(walk->path());
    }
    if (error) {
        throw ArchiveError("a copy of a save could not be read (" + Utf8(here) + ")");
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path& entry : entries) {
        const std::string below = Join(path, Utf8(entry.filename()));
        if (fs::is_directory(entry, error)) {
            if (!store.make_folder(below)) {
                throw ArchiveError("a folder could not be made on the memory stick (" + below +
                                   ")");
            }
            CopyOut(store, entry, below);
        } else if (fs::is_regular_file(entry, error)) {
            if (!store.write(below, ReadNative(entry))) {
                throw ArchiveError("a file could not be written on the memory stick (" +
                                   below + ")");
            }
        }
    }
}

/**
 * After a fetch: the copies of a game's folders go back to the store. Every folder is first
 * written beside its old one (a name no game's folder has: IsSaveFolderOf); only when all of
 * them are there do they take the old ones' places, so that a failure on the way leaves the
 * memory stick's save as it was. The old save is in the backups by then (Unpack).
 */
void WriteBack(const Store& store, const std::vector<Folder>& placed) {
    constexpr const char* NewSuffix = ".savesync-new";
    constexpr const char* OldSuffix = ".savesync-old";
    const std::vector<Store::Entry> top = StoreTop(store);
    const auto there = [&](const std::string& name) {
        return std::any_of(top.begin(), top.end(), [&](const Store::Entry& entry) {
            return entry.folder && entry.name == name;
        });
    };
    std::vector<std::string> written;
    const auto drop_written = [&] {
        for (const std::string& name : written) {
            store.remove(name + NewSuffix);
        }
    };
    try {
        for (const Folder& folder : placed) {
            std::error_code error;
            if (!fs::is_directory(folder.path, error)) {
                continue;
            }
            const std::string fresh = folder.label + NewSuffix;
            store.remove(fresh);
            written.push_back(folder.label);
            if (!store.make_folder(fresh)) {
                throw ArchiveError("a folder could not be made on the memory stick (" + fresh +
                                   ")");
            }
            CopyOut(store, folder.path, fresh);
        }
    } catch (...) {
        drop_written();
        throw;
    }
    for (const Folder& folder : placed) {
        const std::string& name = folder.label;
        const bool fresh = std::find(written.begin(), written.end(), name) != written.end();
        if (!fresh) {
            // The save fetched has nothing for it.
            if (there(name) && !store.remove(name)) {
                throw ArchiveError("a folder of the old save could not be removed from the "
                                   "memory stick (" + name + ")");
            }
            continue;
        }
        const std::string old = name + OldSuffix;
        const bool had = there(name);
        if (had) {
            store.remove(old);
            if (!store.rename(name, old)) {
                drop_written();
                throw ArchiveError("the old save could not be moved aside on the memory stick (" +
                                   name + ")");
            }
        }
        if (!store.rename(name + NewSuffix, name)) {
            if (had) {
                store.rename(old, name);
            }
            drop_written();
            throw ArchiveError("the save could not be put in place on the memory stick (" +
                               name + ")");
        }
        if (had) {
            store.remove(old);
        }
    }
}

std::vector<std::string> DiscsIn(const std::vector<std::string>& folders) {
    std::vector<std::string> discs;
    for (const std::string& folder : folders) {
        if (folder.size() < 9) {
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
    if (shared.store) {
        const Store store = shared.store;
        game.locate = [store, savedata, disc] { return CopyInGame(store, savedata, disc); };
        game.unpacked = [store](const std::vector<Folder>& placed) { WriteBack(store, placed); };
    }
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

void LoadSettings(Shared& shared, const std::string& device_name,
                  const std::vector<std::string>& settings_from) {
    shared.settings.device = device_name;
    std::ifstream in{shared.data / "settings.json", std::ios::binary};
    bool carried = false;
    for (const std::string& other : settings_from) {
        if (in) {
            break;
        }
        in = std::ifstream{FromUtf8(other) / "settings.json", std::ios::binary};
        carried = static_cast<bool>(in);
    }
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
    if (carried) {
        SaveSettings(shared);
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
    std::vector<std::string> discs;
    if (shared.store) {
        std::vector<std::string> folders;
        for (const Store::Entry& entry : StoreTop(shared.store)) {
            if (entry.folder) {
                folders.push_back(entry.name);
            }
        }
        discs = DiscsIn(folders);
    } else {
        discs = DiscsWithSaves(Utf8(shared.savedata));
    }
    std::vector<Game> games;
    for (const std::string& disc : discs) {
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
    std::vector<std::string> folders;
    std::error_code error;
    for (fs::directory_iterator walk{FromUtf8(savedata), error}, end; !error && walk != end;
         walk.increment(error)) {
        if (walk->is_directory(error)) {
            folders.push_back(Utf8(walk->path().filename()));
        }
    }
    return DiscsIn(folders);
}

void Start(const std::string& savedata, const std::string& data,
           std::function<void(const std::string&)> say, const std::string& device_name,
           Store store, const std::vector<std::string>& settings_from) {
    // What ran before, on another memory stick: nothing of it may reach this one.
    Shutdown();
    try {
        auto shared = std::make_shared<Shared>();
        shared->data = FromUtf8(data);
        shared->store = std::move(store);
        shared->savedata = shared->store ? shared->data / "staging" : FromUtf8(savedata);
        shared->say = std::move(say);
        if (shared->store) {
            std::error_code error;
            fs::create_directories(shared->savedata, error);
        }
        LoadSettings(*shared, device_name, settings_from);
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

bool Staged() {
    const auto shared = Current();
    return shared && shared->store;
}

std::string DataFolder() {
    const auto shared = Current();
    return shared ? Utf8(shared->data) : std::string{};
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
    } catch (const ArchiveError& error) {
        SetStatus(*shared, std::string{"Save sync: "} + error.what() + ".");
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
