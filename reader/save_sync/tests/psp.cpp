// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Save sync as the emulator uses it (host.h): a PSP game's save is every folder of
// PSP/SAVEDATA that begins with the game's ID. Made-up saves; the tests that need a service
// are skipped without one (tools/reader/test-savesync.sh starts it).

#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include "reader/save_sync/host.h"
#include "reader/save_sync/tests/test_util.h"

using namespace SaveSyncTest;
namespace Host = SaveSync::Host;

namespace {

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string NewDisc() {
    static std::mt19937 random{std::random_device{}()};
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "ZZTS%05u", static_cast<unsigned>(random() % 100000));
    return buffer;
}

/// An install of the app: a memory stick's SAVEDATA and save sync's own folder.
struct Install {
    explicit Install(std::string name_) : name{std::move(name_)} {}

    void Start(bool enabled = true) {
        Host::Start((scratch.path / "SAVEDATA").string(), (scratch.path / "sync").string(),
                    [this](const std::string& text) {
                        std::scoped_lock lock{mutex};
                        said.push_back(text);
                    },
                    name);
        Host::Settings settings = Host::Get();
        settings.enabled = enabled;
        settings.addresses = Env("SAVESYNC_TEST_ADDRESS");
        settings.code = Env("SAVESYNC_TEST_CODE");
        Host::Set(settings);
    }
    void Save(const std::string& folder, const std::string& content) {
        Write(scratch.path / "SAVEDATA" / folder / "DATA.BIN", content);
        Host::NoteSaved();
    }
    [[nodiscard]] std::map<std::string, std::string> Saves() const {
        return Tree(scratch.path / "SAVEDATA");
    }
    [[nodiscard]] std::vector<std::string> Said() {
        std::scoped_lock lock{mutex};
        return said;
    }

    std::string name;
    Scratch scratch;
    std::mutex mutex;
    std::vector<std::string> said;
};

/// What Android's file picker gives for a memory stick's SAVEDATA: no path of this system's.
constexpr const char* TreeSavedata =
    "content://com.android.externalstorage.documents/tree/primary%3AReader%20Games%2FPSP/"
    "document/primary%3AReader%20Games%2FPSP%2FPSP%2FSAVEDATA";

/// A memory stick that is a document tree, as save sync reaches it: through a Store, here
/// kept in memory, so that any use of the file system for it would find nothing.
struct DocumentTree {
    std::mutex mutex;
    std::set<std::string> folders;
    std::map<std::string, std::string> files;
    bool fail_list = false;
    bool fail_write = false;
    bool fail_rename = false;

    static std::string Parent(const std::string& path) {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    }
    bool IsFolder(const std::string& path) const {
        return path.empty() || folders.contains(path);
    }
    static bool Within(const std::string& path, const std::string& folder) {
        return path == folder || path.starts_with(folder + "/");
    }

    void Put(const std::string& path, const std::string& content) {
        std::scoped_lock lock{mutex};
        for (std::string folder = Parent(path); !folder.empty(); folder = Parent(folder)) {
            folders.insert(folder);
        }
        files[path] = content;
    }
    void Drop(const std::string& folder) {
        std::scoped_lock lock{mutex};
        std::erase_if(folders, [&](const std::string& f) { return Within(f, folder); });
        std::erase_if(files, [&](const auto& f) { return Within(f.first, folder); });
    }
    std::map<std::string, std::string> Files() {
        std::scoped_lock lock{mutex};
        return files;
    }
    std::set<std::string> Folders() {
        std::scoped_lock lock{mutex};
        return folders;
    }

    static Host::Store StoreOf(const std::shared_ptr<DocumentTree>& tree) {
        Host::Store store;
        store.list = [tree](const std::string& path, std::vector<Host::Store::Entry>& out) {
            std::scoped_lock lock{tree->mutex};
            if (tree->fail_list || !tree->IsFolder(path)) {
                return false;
            }
            out.clear();
            for (const std::string& folder : tree->folders) {
                if (Parent(folder) == path) {
                    out.push_back({folder.substr(path.empty() ? 0 : path.size() + 1), true, 0});
                }
            }
            for (const auto& [file, content] : tree->files) {
                if (Parent(file) == path) {
                    out.push_back(
                        {file.substr(path.empty() ? 0 : path.size() + 1), false, 1700000000000});
                }
            }
            return true;
        };
        store.read = [tree](const std::string& path, std::string& content) {
            std::scoped_lock lock{tree->mutex};
            const auto found = tree->files.find(path);
            if (tree->fail_list || found == tree->files.end()) {
                return false;
            }
            content = found->second;
            return true;
        };
        store.write = [tree](const std::string& path, std::string_view content) {
            std::scoped_lock lock{tree->mutex};
            if (tree->fail_write || !tree->IsFolder(Parent(path))) {
                return false;
            }
            tree->files[path] = std::string{content};
            return true;
        };
        store.make_folder = [tree](const std::string& path) {
            std::scoped_lock lock{tree->mutex};
            if (tree->fail_write || !tree->IsFolder(Parent(path))) {
                return false;
            }
            tree->folders.insert(path);
            return true;
        };
        store.remove = [tree](const std::string& path) {
            std::scoped_lock lock{tree->mutex};
            std::erase_if(tree->folders, [&](const std::string& f) { return Within(f, path); });
            std::erase_if(tree->files, [&](const auto& f) { return Within(f.first, path); });
            return true;
        };
        store.rename = [tree](const std::string& path, const std::string& name) {
            std::scoped_lock lock{tree->mutex};
            if (tree->fail_rename || !tree->folders.contains(path) || tree->IsFolder(name) ||
                tree->files.contains(name) || name.find('/') != std::string::npos) {
                return false;
            }
            const auto moved = [&](const std::string& p) { return name + p.substr(path.size()); };
            std::set<std::string> folders;
            for (const std::string& f : tree->folders) {
                folders.insert(Within(f, path) ? moved(f) : f);
            }
            std::map<std::string, std::string> files;
            for (const auto& [f, content] : tree->files) {
                files[Within(f, path) ? moved(f) : f] = content;
            }
            tree->folders = std::move(folders);
            tree->files = std::move(files);
            return true;
        };
        return store;
    }
};

/// An install whose memory stick is a document tree (the Fold's).
struct TreeInstall {
    explicit TreeInstall(std::string name_) : name{std::move(name_)} {}

    void Start(bool enabled = true) {
        Host::Start(TreeSavedata, (scratch.path / "sync").string(),
                    [this](const std::string& text) {
                        std::scoped_lock lock{mutex};
                        said.push_back(text);
                    },
                    name, DocumentTree::StoreOf(tree));
        Host::Settings settings = Host::Get();
        settings.enabled = enabled;
        settings.addresses = Env("SAVESYNC_TEST_ADDRESS");
        settings.code = Env("SAVESYNC_TEST_CODE");
        Host::Set(settings);
    }
    void Save(const std::string& folder, const std::string& content) {
        tree->Put(folder + "/DATA.BIN", content);
        Host::NoteSaved();
    }
    [[nodiscard]] std::map<std::string, std::string> Saves() const {
        return tree->Files();
    }
    /// Folders save sync leaves on the memory stick while it replaces a save.
    [[nodiscard]] bool LeftOvers() const {
        for (const std::string& folder : tree->Folders()) {
            if (folder.find(".savesync") != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::vector<std::string> Said() {
        std::scoped_lock lock{mutex};
        return said;
    }

    std::string name;
    Scratch scratch;
    std::shared_ptr<DocumentTree> tree = std::make_shared<DocumentTree>();
    std::mutex mutex;
    std::vector<std::string> said;
};

#define NEEDS_SERVICE()                                                                           \
    if (Env("SAVESYNC_TEST_ADDRESS").empty()) {                                                   \
        SKIP("no service: run tools/reader/test-savesync.sh");                                    \
    }

} // namespace

TEST_CASE("A PSP game's ID is a title of the service's kind, and back", "[savesync][psp]") {
    CHECK(Host::TitleOf("ULJM05887") == "554C4A4D00005887");
    CHECK(Host::DiscOf("554C4A4D00005887") == "ULJM05887");
    CHECK(Host::TitleOf("NPJH50001") == "4E504A4800050001");
    CHECK(Host::TitleOf("uljm05887").empty());
    CHECK(Host::TitleOf("ULJM0588").empty());
    CHECK(Host::TitleOf("ULJM0588A").empty());
    CHECK(Host::TitleOf("HOMEBREW1").empty());
    CHECK(Host::DiscOf("0100CB000A142000").empty());
    CHECK(Host::DiscOf("554C4A4D00105887").empty());
    CHECK(Host::DiscOf("554C").empty());
}

TEST_CASE("Only a game's own folders are its save", "[savesync][psp]") {
    CHECK(Host::IsSaveFolderOf("ULJM05887", "ULJM05887"));
    CHECK(Host::IsSaveFolderOf("ULJM05887", "ULJM05887DATA00"));
    CHECK(Host::IsSaveFolderOf("ULJM05887", "ULJM05887_SYS-1"));
    CHECK_FALSE(Host::IsSaveFolderOf("ULJM05887", "ULJM05888DATA00"));
    // What save sync itself puts beside a folder while it replaces it.
    CHECK_FALSE(Host::IsSaveFolderOf("ULJM05887", "ULJM05887DATA00.savesync-new"));
    CHECK_FALSE(Host::IsSaveFolderOf("ULJM05887", "ULJM05887/../x"));
    CHECK_FALSE(Host::IsSaveFolderOf("ULJM05887", "ULJM05887 copy"));
    CHECK_FALSE(Host::IsSaveFolderOf("HOMEBREW1", "HOMEBREW1"));

    Scratch scratch;
    Write(scratch.path / "ULJM05887DATA00" / "DATA.BIN", "a");
    Write(scratch.path / "ULJM05887SYS" / "DATA.BIN", "b");
    Write(scratch.path / "ULJS00219" / "DATA.BIN", "c");
    Write(scratch.path / "notes.txt", "d");
    Write(scratch.path / "My Homebrew" / "DATA.BIN", "e");
    CHECK(Host::DiscsWithSaves(scratch.path.string()) ==
          std::vector<std::string>{"ULJM05887", "ULJS00219"});
}

TEST_CASE("Switched off, nothing is sent and nothing is said", "[savesync][psp]") {
    Install thor{"Thor"};
    thor.Start(false);
    const std::string disc = NewDisc();
    thor.Save(disc + "DATA00", "chapter 1");
    Host::GameStarting(disc, "A made-up game");
    CHECK(Host::RunningDisc() == disc);
    thor.Save(disc + "DATA00", "chapter 2");
    Host::GameStopped();
    CHECK(Host::RunningDisc().empty());
    CHECK(Host::Wait(5000));
    CHECK(thor.Said().empty());
    CHECK(Host::Conflicts().empty());
    Host::Shutdown();
    CHECK_FALSE(Host::Started());
}

TEST_CASE("The settings are kept from one start to the next", "[savesync][psp]") {
    Install thor{"Thor"};
    Host::Start((thor.scratch.path / "SAVEDATA").string(), (thor.scratch.path / "sync").string(),
                {}, "Thor");
    CHECK(Host::Get().device == "Thor");
    CHECK_FALSE(Host::Get().enabled);
    Host::Settings settings;
    settings.enabled = true;
    settings.addresses = "127.0.0.1:1";
    settings.code = "AAAA-BBBB-CCCC";
    settings.device = "My Thor";
    Host::Set(settings);
    Host::Shutdown();
    Host::Start((thor.scratch.path / "SAVEDATA").string(), (thor.scratch.path / "sync").string(),
                {}, "Another name");
    const Host::Settings kept = Host::Get();
    CHECK(kept.enabled);
    CHECK(kept.addresses == "127.0.0.1:1");
    CHECK(kept.code == "AAAA-BBBB-CCCC");
    CHECK(kept.device == "My Thor");
    Host::Shutdown();
}

TEST_CASE("A PSP save goes to another device with every folder of it", "[savesync][psp]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    Install thor{"Thor"};
    Install z13{"Z13"};

    thor.Start();
    thor.Save(disc + "DATA00", "chapter 1");
    thor.Save(disc + "SYS", "settings");
    thor.Save("ULJS99999", "another game's");
    Host::GameStarting(disc, "A made-up game");
    REQUIRE(Host::Wait(10000));
    // While it runs: a save in a folder that was not there at the start.
    thor.Save(disc + "DATA01", "chapter 2");
    REQUIRE(Host::Wait(10000));
    Host::GameStopped();
    Host::Shutdown();

    z13.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(z13.Saves() == std::map<std::string, std::string>{
                             {disc + "DATA00/DATA.BIN", "chapter 1"},
                             {disc + "DATA01/DATA.BIN", "chapter 2"},
                             {disc + "SYS/DATA.BIN", "settings"}});
    // And back: a folder deleted there is gone here, kept in the backups.
    std::filesystem::remove_all(z13.scratch.path / "SAVEDATA" / (disc + "DATA00"));
    z13.Save(disc + "DATA01", "chapter 3");
    REQUIRE(Host::Wait(10000));
    Host::GameStopped();
    Host::Shutdown();

    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(thor.Saves() == std::map<std::string, std::string>{
                              {disc + "DATA01/DATA.BIN", "chapter 3"},
                              {disc + "SYS/DATA.BIN", "settings"},
                              {"ULJS99999/DATA.BIN", "another game's"}});
    const auto backups = Tree(thor.scratch.path / "sync" / "backups" / Host::TitleOf(disc));
    CHECK(backups.size() == 3);
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("Two devices both played: the user chooses, in the menu", "[savesync][psp]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    const std::string title = Host::TitleOf(disc);
    Install thor{"Thor"};
    Install z13{"Z13"};

    thor.Start();
    thor.Save(disc + "DATA00", "chapter 1");
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    z13.Start();
    Host::GameStarting(disc, "A made-up game");
    REQUIRE(z13.Saves().size() == 1);
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    // The Z13 plays on without the service (the app is not even running here: started with
    // sync on, it would look at every save at once)...
    Host::Shutdown();
    z13.Save(disc + "DATA00", "the Z13's chapter 2");

    // ...and so does the Thor, which reaches it first.
    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    thor.Save(disc + "DATA00", "the Thor's chapter 2");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // Back at the service, the Z13 is told and keeps its own save for now.
    z13.Start();
    Host::GameStarting(disc, "A made-up game");
    REQUIRE(Host::Conflicts().size() == 1);
    CHECK(Host::Conflicts()[0].title == title);
    CHECK(Host::Conflicts()[0].disc == disc);
    CHECK(Host::Conflicts()[0].name == "A made-up game");
    CHECK(Host::Conflicts()[0].message.find("Thor: saved ") != std::string::npos);
    // Said now that the game starts (and before, when the app's start found it).
    REQUIRE_FALSE(z13.Said().empty());
    CHECK(z13.Said().back().find("A made-up game has two different saves") !=
          std::string::npos);
    CHECK(z13.Saves().at(disc + "DATA00/DATA.BIN") == "the Z13's chapter 2");
    // Nothing of it goes up meanwhile.
    z13.Save(disc + "DATA00", "the Z13's chapter 3");
    REQUIRE(Host::Wait(10000));

    // The other save cannot take this one's place under a running game.
    Host::Resolve(title, false);
    REQUIRE(Host::Wait(10000));
    CHECK(Host::Status().find("Stop the game first") == 0);
    CHECK(z13.Saves().at(disc + "DATA00/DATA.BIN") == "the Z13's chapter 3");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    CHECK(Host::Conflicts().size() == 1);

    Host::Resolve(title, false);
    REQUIRE(Host::Wait(10000));
    CHECK(Host::Conflicts().empty());
    CHECK(z13.Saves().at(disc + "DATA00/DATA.BIN") == "the Thor's chapter 2");
    const auto backups = Tree(z13.scratch.path / "sync" / "backups" / title);
    REQUIRE(backups.size() == 1);
    CHECK(backups.begin()->second == "the Z13's chapter 3");
    Host::Shutdown();

    // The Thor still has its own, and nothing to choose.
    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(Host::Conflicts().empty());
    CHECK(thor.Saves().at(disc + "DATA00/DATA.BIN") == "the Thor's chapter 2");
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("A conflict found when the app starts is listed, and this device's save can be kept",
          "[savesync][psp]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    const std::string title = Host::TitleOf(disc);
    Install thor{"Thor"};
    Install z13{"Z13"};

    thor.Start();
    thor.Save(disc, "chapter 1");
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();
    z13.Start();
    Host::GameStarting(disc, "");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();
    z13.Save(disc, "the Z13's chapter 2");
    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    thor.Save(disc, "the Thor's chapter 2");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // The app's start looks at every game with a save here.
    z13.Start();
    REQUIRE(Host::Wait(10000));
    REQUIRE(Host::Conflicts().size() == 1);
    CHECK(Host::Conflicts()[0].disc == disc);
    Host::Resolve(title, true);
    REQUIRE(Host::Wait(10000));
    CHECK(Host::Conflicts().empty());
    CHECK(z13.Saves().at(disc + "/DATA.BIN") == "the Z13's chapter 2");
    Host::Shutdown();

    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(thor.Saves().at(disc + "/DATA.BIN") == "the Z13's chapter 2");
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("What the background did names its game", "[savesync][psp]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    Install thor{"Thor"};
    thor.Save(disc, "chapter 1");
    thor.Start();
    Host::SyncAll();
    REQUIRE(Host::Wait(10000));
    REQUIRE_FALSE(thor.Said().empty());
    CHECK(thor.Said().back() == "Save sync: this device's save was sent. (" + disc + ")");
    CHECK(Host::Status() == thor.Said().back());

    // Once the game has been started here its name is known; and what is said at its own
    // start needs no name.
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();
    thor.Save(disc, "chapter 2");
    thor.Start();
    REQUIRE(Host::Wait(10000));
    CHECK(thor.Said().back() == "Save sync: this device's save was sent. (A made-up game)");
    Host::Shutdown();
}

TEST_CASE("A memory stick that only Android's file picker reaches: saves go up and come back",
          "[savesync][psp][tree]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    Install thor{"Thor"};
    TreeInstall fold{"Fold"};

    thor.Start();
    thor.Save(disc + "DATA00", "chapter 1");
    thor.Save(disc + "SYS", "settings");
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // The other device's save is fetched before the game runs, onto the memory stick.
    fold.tree->Put("ULJS99999/DATA.BIN", "another game's");
    fold.Start();
    REQUIRE(Host::Wait(10000));
    CHECK(Host::Staged());
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{
                              {disc + "DATA00/DATA.BIN", "chapter 1"},
                              {disc + "SYS/DATA.BIN", "settings"},
                              {"ULJS99999/DATA.BIN", "another game's"}});
    CHECK_FALSE(fold.LeftOvers());
    // What the game writes there goes up: a new folder, a folder deleted.
    fold.Save(disc + "DATA01", "chapter 2");
    fold.tree->Drop(disc + "DATA00");
    REQUIRE(Host::Wait(10000));
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(thor.Saves() == std::map<std::string, std::string>{
                              {disc + "DATA01/DATA.BIN", "chapter 2"},
                              {disc + "SYS/DATA.BIN", "settings"}});
    thor.Save(disc + "DATA01", "chapter 3");
    thor.Save(disc + "DATA02", "chapter 3, the other slot");
    REQUIRE(Host::Wait(10000));
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // A save that replaces one on the memory stick: the old one is in the backups.
    fold.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{
                              {disc + "DATA01/DATA.BIN", "chapter 3"},
                              {disc + "DATA02/DATA.BIN", "chapter 3, the other slot"},
                              {disc + "SYS/DATA.BIN", "settings"},
                              {"ULJS99999/DATA.BIN", "another game's"}});
    CHECK_FALSE(fold.LeftOvers());
    const auto backups = Tree(fold.scratch.path / "sync" / "backups" / Host::TitleOf(disc));
    CHECK(backups.size() == 2);
    // Nothing for the other game was fetched, and nothing touched the file system's idea of
    // the document tree's address.
    CHECK_FALSE(std::filesystem::exists("content:"));
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("A memory stick that cannot be read or written keeps its save",
          "[savesync][psp][tree]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    Install thor{"Thor"};
    TreeInstall fold{"Fold"};

    thor.Start();
    thor.Save(disc, "chapter 1");
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // Unreadable (the app lost its access): it is not taken for a memory stick without a save.
    fold.tree->Put(disc + "/DATA.BIN", "the Fold's own");
    fold.tree->fail_list = true;
    fold.Start();
    REQUIRE(Host::Wait(10000));
    Host::GameStarting(disc, "A made-up game");
    REQUIRE_FALSE(fold.Said().empty());
    CHECK(fold.Said().back().find("could not be read") != std::string::npos);
    fold.tree->fail_list = false;
    CHECK(fold.Saves() == std::map<std::string, std::string>{{disc + "/DATA.BIN", "the Fold's own"}});
    Host::GameStopped();
    Host::Shutdown();
    fold.tree->Drop(disc);

    // Not writable: the fetch fails and leaves nothing behind; the next start takes it.
    fold.tree->fail_write = true;
    fold.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves().empty());
    CHECK_FALSE(fold.LeftOvers());
    CHECK(Host::Status().find("memory stick") != std::string::npos);
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    fold.tree->fail_write = false;
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{{disc + "/DATA.BIN", "chapter 1"}});
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // The old save cannot be moved aside: it stays as it was.
    thor.Start();
    Host::GameStarting(disc, "A made-up game");
    thor.Save(disc, "chapter 2");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();
    fold.tree->fail_rename = true;
    fold.Start();
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{{disc + "/DATA.BIN", "chapter 1"}});
    CHECK_FALSE(fold.LeftOvers());
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    fold.tree->fail_rename = false;
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{{disc + "/DATA.BIN", "chapter 2"}});
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("Started again for another memory stick, save sync uses only that one",
          "[savesync][psp][tree]") {
    NEEDS_SERVICE();
    const std::string disc = NewDisc();
    Install thor{"Thor"};
    thor.Start();
    thor.Save(disc, "chapter 1");
    Host::GameStarting(disc, "A made-up game");
    Host::GameStopped();
    REQUIRE(Host::Wait(10000));
    Host::Shutdown();

    // The app's own folder first, then a folder chosen with the file picker while it runs.
    Install before{"Fold"};
    TreeInstall fold{"Fold"};
    before.Start();
    REQUIRE(Host::Wait(10000));
    fold.Start();
    CHECK(Host::Staged());
    Host::GameStarting(disc, "A made-up game");
    CHECK(fold.Saves() == std::map<std::string, std::string>{{disc + "/DATA.BIN", "chapter 1"}});
    CHECK(before.Saves().empty());
    Host::GameStopped();
    Host::Shutdown();
}

TEST_CASE("The settings come along to a new memory stick", "[savesync][psp][tree]") {
    Install old_stick{"Fold"};
    TreeInstall fold{"Fold"};
    const std::string old_data = (old_stick.scratch.path / "sync").string();
    Host::Start((old_stick.scratch.path / "SAVEDATA").string(), old_data, {}, "Fold");
    Host::Settings settings;
    settings.enabled = true;
    settings.addresses = "127.0.0.1:1";
    settings.code = "AAAA-BBBB-CCCC";
    settings.device = "My Fold";
    Host::Set(settings);

    const std::string data = (fold.scratch.path / "sync").string();
    Host::Start(TreeSavedata, data, {}, "Another name", DocumentTree::StoreOf(fold.tree),
                {(fold.scratch.path / "nothing here").string(), old_data});
    CHECK(Host::DataFolder() == data);
    Host::Settings kept = Host::Get();
    CHECK(kept.enabled);
    CHECK(kept.addresses == "127.0.0.1:1");
    CHECK(kept.code == "AAAA-BBBB-CCCC");
    CHECK(kept.device == "My Fold");
    // Kept in the new folder: changed there, the old memory stick's do not come back.
    kept.device = "The Fold";
    Host::Set(kept);
    Host::Shutdown();
    Host::Start(TreeSavedata, data, {}, "Another name", DocumentTree::StoreOf(fold.tree), {old_data});
    CHECK(Host::Get().device == "The Fold");
    Host::Shutdown();
    CHECK(Host::DataFolder().empty());
}
