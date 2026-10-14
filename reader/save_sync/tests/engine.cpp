// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

// The engine against a real service: the standalone one (the Eden Reader fork's
// tools/reader/savesync/server.py), which tools/reader/test-savesync.sh starts on a loopback
// port with a folder of its own and names in SAVESYNC_TEST_ADDRESS and SAVESYNC_TEST_CODE
// (and a second one in SAVESYNC_TEST_OTHER_ADDRESS and SAVESYNC_TEST_OTHER_CODE). Without
// them these tests are skipped. Each test has a game of its own, so they do not meet there.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "reader/save_sync/engine.h"
#include "reader/save_sync/tests/test_util.h"

using namespace SaveSync;
using namespace SaveSyncTest;
using Kind = Result::Kind;

namespace {

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string NewTitle() {
    static std::mt19937_64 random{std::random_device{}()};
    return TitleText(random() | 0x0100000000000000ULL);
}

/// An install of the app: its save folders, its notes and its engine.
struct Device {
    Device(const std::string& name, const std::string& title, std::string addresses = {},
           std::string code = {}) {
        config.enabled = true;
        config.addresses = addresses.empty() ? Env("SAVESYNC_TEST_ADDRESS") : addresses;
        config.code = code.empty() ? Env("SAVESYNC_TEST_CODE") : code;
        config.device = name;
        config.state_file = scratch.path / "notes" / "save-sync.json";
        config.backup_dir = scratch.path / "backups";
        engine.Configure(config);
        engine.SetListener([this](const Result& result) {
            std::scoped_lock lock{mutex};
            heard.push_back(result);
        });
        game.title = title;
        game.name = "A made-up game";
        game.folders = Folders(scratch.path / "nand");
    }

    void Save(const std::string& content, const std::string& file = "save.dat") {
        Write(game.folders[0].path / file, content);
    }
    [[nodiscard]] std::string Saved(const std::string& file = "save.dat") const {
        return Read(game.folders[0].path / file);
    }
    [[nodiscard]] std::vector<Result> Heard() {
        std::scoped_lock lock{mutex};
        return heard;
    }
    /// Waits until the background reported something of this kind.
    [[nodiscard]] bool Hears(Kind kind, std::chrono::seconds within = std::chrono::seconds(10)) {
        const auto until = std::chrono::steady_clock::now() + within;
        while (std::chrono::steady_clock::now() < until) {
            for (const Result& result : Heard()) {
                if (result.kind == kind) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    [[nodiscard]] std::vector<std::filesystem::path> Backups() const {
        std::vector<std::filesystem::path> found;
        const auto folder = config.backup_dir / game.title;
        if (std::filesystem::exists(folder)) {
            for (const auto& entry : std::filesystem::directory_iterator{folder}) {
                found.push_back(entry.path());
            }
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    Scratch scratch;
    Config config;
    Game game;
    std::mutex mutex;
    std::vector<Result> heard;
    // Last: its thread ends before what the listener uses.
    Engine engine;
};

bool NoService() {
    return Env("SAVESYNC_TEST_ADDRESS").empty();
}

#define NEEDS_SERVICE()                                                                           \
    if (NoService()) {                                                                            \
        SKIP("no service: run tools/reader/test-savesync.sh");                                    \
    }

} // namespace

TEST_CASE("The address and the code are checked", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device good{"z13", title};
    const Result fine = good.engine.Check();
    CHECK(fine.kind == Kind::InSync);
    CHECK(fine.message.find("Connected to " + Env("SAVESYNC_TEST_ADDRESS")) == 0);

    Device wrong_code{"z13", title, {}, "AAAA-BBBB-CCCC"};
    const Result refused = wrong_code.engine.Check();
    CHECK(refused.kind == Kind::Failed);
    CHECK(refused.message == "Save sync: the code is not the service's.");

    // Nothing listens on port 1.
    Device nowhere{"z13", title, "127.0.0.1:1"};
    CHECK(nowhere.engine.Check().kind == Kind::Offline);

    Device unset{"z13", title, "  ,  "};
    CHECK(unset.engine.Check().kind == Kind::Off);
    CHECK_FALSE(unset.engine.IsOn());
    CHECK(good.engine.IsOn());

    // The code as it is typed: small letters, spaces.
    std::string typed = Env("SAVESYNC_TEST_CODE");
    for (char& c : typed) {
        c = c == '-' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    Device typed_code{"z13", title, {}, typed};
    CHECK(typed_code.engine.Check().kind == Kind::InSync);
}

TEST_CASE("A save goes from one device to the other and back", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};

    // Nothing anywhere yet.
    CHECK(thor.engine.Prepare(thor.game).kind == Kind::InSync);
    thor.engine.Release();

    thor.Save("chapter 1");
    Write(thor.game.folders[0].path / "sub" / "extra.bin", "more");
    Write(thor.game.folders[1].path / "device.dat", "shared");
    CHECK(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    thor.engine.Release();

    const Result fetched = z13.engine.Prepare(z13.game);
    CHECK(fetched.kind == Kind::Fetched);
    CHECK(fetched.theirs.device == "Thor");
    CHECK(fetched.message.find("Save sync: took the save made on Thor (20") == 0);
    CHECK(Tree(z13.game.folders[0].path) == Tree(thor.game.folders[0].path));
    CHECK(Tree(z13.game.folders[1].path) == Tree(thor.game.folders[1].path));
    CHECK(z13.Backups().empty());
    z13.engine.Release();
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::InSync);
    z13.engine.Release();

    // Played on the Z13, then back on the Thor.
    z13.Save("chapter 2");
    std::filesystem::remove(z13.game.folders[0].path / "sub" / "extra.bin");
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::Sent);
    CHECK(thor.engine.Prepare(thor.game).kind == Kind::Fetched);
    CHECK(thor.Saved() == "chapter 2");
    CHECK_FALSE(std::filesystem::exists(thor.game.folders[0].path / "sub" / "extra.bin"));
    // The Thor's save of before is kept on the Thor.
    REQUIRE(thor.Backups().size() == 1);
    CHECK(Read(thor.Backups()[0] / "user" / "save.dat") == "chapter 1");
    CHECK(Read(thor.Backups()[0] / "user" / "sub" / "extra.bin") == "more");

    TitleInfo history;
    CHECK(thor.engine.History(title, history).kind == Kind::InSync);
    CHECK(history.name == "A made-up game");
    REQUIRE(history.versions.size() == 2);
    CHECK(history.versions[0].device == "Z13");
    CHECK(history.versions[1].device == "Thor");
    CHECK(history.head->n == 2);
}

TEST_CASE("Both devices played: the user is asked, and neither save is lost",
          "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};
    thor.Save("start");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    REQUIRE(z13.engine.Prepare(z13.game).kind == Kind::Fetched);

    thor.Save("played on the Thor");
    z13.Save("played on the Z13");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);

    const Result asked = z13.engine.Prepare(z13.game);
    REQUIRE(asked.kind == Kind::Conflict);
    CHECK(asked.theirs.device == "Thor");
    CHECK(asked.theirs.n == 2);
    CHECK(asked.ours.files == 1);
    CHECK(asked.ours.newest_ms > 0);
    CHECK(z13.Saved() == "played on the Z13");

    SECTION("the other device's save is taken") {
        const Result result = z13.engine.Resolve(z13.game, false);
        CHECK(result.kind == Kind::Fetched);
        CHECK(z13.Saved() == "played on the Thor");
        REQUIRE(z13.Backups().size() == 1);
        CHECK(Read(z13.Backups()[0] / "user" / "save.dat") == "played on the Z13");
        // And the service has the Z13's save beside the versions.
        TitleInfo history;
        REQUIRE(z13.engine.History(title, history).kind == Kind::InSync);
        CHECK(history.head->n == 2);
        REQUIRE(history.versions.size() == 3);
        CHECK(history.versions[0].kind == "side");
        CHECK(history.versions[0].device == "Z13");
        CHECK(z13.engine.Prepare(z13.game).kind == Kind::InSync);
        CHECK(thor.engine.Prepare(thor.game).kind == Kind::InSync);
    }

    SECTION("this device's save is kept") {
        const Result result = z13.engine.Resolve(z13.game, true);
        CHECK(result.kind == Kind::Sent);
        CHECK(z13.Saved() == "played on the Z13");
        // The Thor did not play since: it gets the Z13's without a question, and keeps its own.
        CHECK(thor.engine.Prepare(thor.game).kind == Kind::Fetched);
        CHECK(thor.Saved() == "played on the Z13");
        REQUIRE(thor.Backups().size() == 1);
        CHECK(Read(thor.Backups()[0] / "user" / "save.dat") == "played on the Thor");
        TitleInfo history;
        REQUIRE(z13.engine.History(title, history).kind == Kind::InSync);
        REQUIRE(history.versions.size() == 3);
        CHECK(history.versions[1].device == "Thor");
    }

    SECTION("left open: nothing is sent, and the question comes again") {
        z13.engine.Skip(title);
        std::atomic<std::uint64_t> commits{0};
        z13.engine.Watch(z13.game, [&] { return commits.load(); });
        z13.Save("played on, on the Z13");
        ++commits;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        z13.engine.Unwatch();
        CHECK(z13.engine.Flush(std::chrono::seconds(10)));
        TitleInfo history;
        REQUIRE(z13.engine.History(title, history).kind == Kind::InSync);
        CHECK(history.versions.size() == 2);
        CHECK(z13.Heard().empty());
        CHECK(z13.engine.Prepare(z13.game).kind == Kind::Conflict);
    }
}

TEST_CASE("A device new to the service with a save of its own is asked", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device fold{"Fold", title};
    thor.Save("the Thor's");
    fold.Save("the Fold's");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    const Result asked = fold.engine.Prepare(fold.game);
    CHECK(asked.kind == Kind::Conflict);
    CHECK(fold.Saved() == "the Fold's");
}

TEST_CASE("The notes survive the app being closed, and a save known to the service needs none",
          "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};
    thor.Save("one");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    REQUIRE(z13.engine.Prepare(z13.game).kind == Kind::Fetched);
    thor.Save("two");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);

    SECTION("the app started again: the notes are read from their file") {
        Engine again;
        again.Configure(z13.config);
        CHECK(again.Prepare(z13.game).kind == Kind::Fetched);
        CHECK(z13.Saved() == "two");
    }

    SECTION("the notes lost: the save is still version 1, which the service knows") {
        std::filesystem::remove(z13.config.state_file);
        Engine again;
        again.Configure(z13.config);
        CHECK(again.Prepare(z13.game).kind == Kind::Fetched);
        CHECK(z13.Saved() == "two");
    }
}

TEST_CASE("Notes made with one service are not used with another", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device z13{"Z13", title};
    z13.Save("chapter 1");
    REQUIRE(z13.engine.Prepare(z13.game).kind == Kind::Sent);
    z13.engine.Release();

    // Another computer keeps the saves from now on, and has other ones of this game: two
    // versions, so that its numbers do not match the note either way.
    const std::string there = Env("SAVESYNC_TEST_OTHER_ADDRESS");
    const std::string code = Env("SAVESYNC_TEST_OTHER_CODE");
    if (there.empty()) {
        SKIP("no second service: run tools/reader/test-savesync.sh");
    }
    Device thor{"Thor", title, there, code};
    thor.Save("another story");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    thor.Save("another story, further");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    thor.engine.Release();

    // The Z13's save has not changed since its note, and the note's version is not the
    // current one there: with the service it was made with, that means "fetch". Here it
    // would replace a save the new keeper has never seen.
    const std::string home = z13.config.addresses;
    const std::string home_code = z13.config.code;
    z13.config.addresses = there;
    z13.config.code = code;
    z13.engine.Configure(z13.config);
    const Result asked = z13.engine.Prepare(z13.game);
    CHECK(asked.kind == Kind::Conflict);
    CHECK(asked.theirs.device == "Thor");
    CHECK(z13.Saved() == "chapter 1");
    z13.engine.Skip(title);
    z13.engine.Release();

    // Back with the first one, which has this very save: nothing to ask, nothing to do.
    z13.config.addresses = home;
    z13.config.code = home_code;
    z13.engine.Configure(z13.config);
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::InSync);
    CHECK(z13.Saved() == "chapter 1");

    // And the notes say which service they are about, for the app's next start.
    CHECK(Read(z13.config.state_file).find("\"service\": \"") != std::string::npos);
}

TEST_CASE("While a game runs its save is sent after each time it is made final",
          "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::InSync);
    std::atomic<std::uint64_t> commits{7};
    thor.engine.Watch(thor.game, [&] { return commits.load(); });

    // Written but not made final: nothing goes out.
    thor.Save("slot 1");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(thor.Heard().empty());

    ++commits;
    REQUIRE(thor.Hears(Kind::Sent));
    CHECK(thor.Heard()[0].during_play);
    // A commit that changed nothing sends nothing.
    ++commits;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(thor.Heard().size() == 1);

    // Several in a row: one version with the last state.
    thor.Save("slot 1, later");
    ++commits;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    thor.Save("slot 1, later still");
    ++commits;
    CHECK(thor.engine.Flush(std::chrono::seconds(15)));
    TitleInfo history;
    REQUIRE(thor.engine.History(title, history).kind == Kind::InSync);
    CHECK(history.versions.size() == 2);

    // The game is stopped with a save it did not make final by then (the last one is sent
    // all the same).
    thor.Save("the last save");
    thor.engine.Unwatch();
    CHECK(thor.engine.Flush(std::chrono::seconds(15)));
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::Fetched);
    CHECK(z13.Saved() == "the last save");
}

TEST_CASE("Saved elsewhere while the game runs: nothing is overwritten", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};
    thor.Save("start");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    REQUIRE(z13.engine.Prepare(z13.game).kind == Kind::Fetched);

    // The Thor's game is left running; the Z13 plays and saves.
    std::atomic<std::uint64_t> commits{0};
    thor.engine.Watch(thor.game, [&] { return commits.load(); });
    z13.Save("the Z13 went on");
    REQUIRE(z13.engine.Prepare(z13.game).kind == Kind::Sent);

    thor.Save("the Thor went on too");
    ++commits;
    REQUIRE(thor.Hears(Kind::Later));
    const Result later = thor.Heard()[0];
    CHECK(later.during_play);
    CHECK(later.theirs.device == "Z13");
    CHECK(later.message.find("Save sync: Z13 saved A made-up game in the meantime") == 0);
    // Said once, however often the game saves from here.
    thor.Save("and on");
    ++commits;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(thor.Heard().size() == 1);
    thor.engine.Unwatch();
    CHECK(thor.engine.Flush(std::chrono::seconds(10)));

    TitleInfo history;
    REQUIRE(thor.engine.History(title, history).kind == Kind::InSync);
    CHECK(history.head->device == "Z13");
    CHECK(history.versions.size() == 2);
    CHECK(thor.engine.Prepare(thor.game).kind == Kind::Conflict);
}

TEST_CASE("Without the service the game starts with this device's save, which goes up later",
          "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    // The first address does not answer, as the home network's does not on the road.
    Device thor{"Thor", title, "127.0.0.1:1"};
    thor.Save("played on the train");
    const Result offline = thor.engine.Prepare(thor.game);
    CHECK(offline.kind == Kind::Offline);
    CHECK(thor.Saved() == "played on the train");

    std::atomic<std::uint64_t> commits{0};
    thor.engine.Watch(thor.game, [&] { return commits.load(); });
    thor.Save("played on");
    ++commits;
    thor.Save("and on");
    ++commits;
    std::this_thread::sleep_for(std::chrono::seconds(4));
    // It was said at the start: not again at each save.
    CHECK(thor.Heard().empty());
    thor.engine.Unwatch();
    thor.engine.Flush(std::chrono::seconds(10));

    // Home again: both addresses set, the dead one first.
    thor.config.addresses = "127.0.0.1:1, " + Env("SAVESYNC_TEST_ADDRESS");
    thor.engine.Configure(thor.config);
    CHECK(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    Device z13{"Z13", title};
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::Fetched);
    CHECK(z13.Saved() == "and on");
}

TEST_CASE("The service going away while a game runs is said once", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    thor.Save("at home");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    std::atomic<std::uint64_t> commits{0};
    thor.engine.Watch(thor.game, [&] { return commits.load(); });

    // Out of the house, the game still running.
    thor.config.addresses = "127.0.0.1:1";
    thor.engine.Configure(thor.config);
    thor.Save("on the way");
    ++commits;
    REQUIRE(thor.Hears(Kind::Offline));
    CHECK(thor.Heard()[0].during_play);
    thor.Save("further on the way");
    ++commits;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(thor.Heard().size() == 1);

    // Home again with the game still running: the next save goes up, and the one before it
    // is not a conflict.
    thor.config.addresses = Env("SAVESYNC_TEST_ADDRESS");
    thor.engine.Configure(thor.config);
    thor.Save("home again");
    ++commits;
    REQUIRE(thor.Hears(Kind::Sent));
    thor.engine.Unwatch();
    CHECK(thor.engine.Flush(std::chrono::seconds(10)));
    Device z13{"Z13", title};
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::Fetched);
    CHECK(z13.Saved() == "home again");
}

TEST_CASE("All games are brought in line in the background, questions wait",
          "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string sent = NewTitle();
    const std::string fetched = NewTitle();
    const std::string asked = NewTitle();
    const std::string same = NewTitle();
    Scratch other;
    const auto game = [&](Device& device, const std::string& title) {
        Game result;
        result.title = title;
        result.folders = Folders(device.scratch.path / title);
        return result;
    };
    Device thor{"Thor", sent};
    Device z13{"Z13", sent};

    // On the Thor: a save the service has not, one it has an older state of, one that
    // differs, one that is the same.
    Write(game(thor, sent).folders[0].path / "s", "only on the Thor");
    Write(game(z13, fetched).folders[0].path / "s", "from the Z13");
    Write(game(z13, asked).folders[0].path / "s", "the Z13's");
    Write(game(thor, asked).folders[0].path / "s", "the Thor's");
    Write(game(z13, same).folders[0].path / "s", "the same");
    Write(game(thor, same).folders[0].path / "s", "the same");
    for (const std::string& title : {fetched, asked, same}) {
        REQUIRE(z13.engine.Prepare(game(z13, title)).kind == Kind::Sent);
    }
    z13.engine.Release();

    thor.engine.SyncAll({game(thor, sent), game(thor, fetched), game(thor, asked),
                         game(thor, same)});
    REQUIRE(thor.engine.Flush(std::chrono::seconds(20)));
    const auto heard = thor.Heard();
    REQUIRE(heard.size() == 3);
    CHECK(heard[0].kind == Kind::Sent);
    CHECK(heard[0].title == sent);
    CHECK(heard[1].kind == Kind::Fetched);
    CHECK(heard[1].title == fetched);
    CHECK(heard[2].kind == Kind::Later);
    CHECK(heard[2].title == asked);
    CHECK(Read(game(thor, fetched).folders[0].path / "s") == "from the Z13");
    CHECK(Read(game(thor, asked).folders[0].path / "s") == "the Thor's");

    // The Thor sent its save without the game's name (the background does not know it); the
    // name comes from the first device that starts the game. Signs and other scripts in it
    // arrive as they are.
    TitleInfo unnamed;
    REQUIRE(thor.engine.History(sent, unnamed).kind == Kind::InSync);
    CHECK(unnamed.name.empty());
    Game named = game(thor, sent);
    named.name = "ファタ & Co. + 100% (demo)";
    CHECK(thor.engine.Prepare(named).kind == Kind::InSync);
    thor.engine.Release();
    REQUIRE(thor.engine.History(sent, unnamed).kind == Kind::InSync);
    CHECK(unnamed.name == "ファタ & Co. + 100% (demo)");

    // The overview, as the Thor sees it.
    std::string error;
    const auto rows = thor.engine.Overview(
        [&](const std::string& title) { return game(thor, title).folders; }, error);
    CHECK(error.empty());
    const auto here = [&](const std::string& title) {
        for (const OverviewRow& row : rows) {
            if (row.title == title) {
                return row.here;
            }
        }
        return std::string{"missing"};
    };
    CHECK(here(sent) == "same");
    CHECK(here(fetched) == "same");
    CHECK(here(asked) == "differs");
    CHECK(here(same) == "same");
}

TEST_CASE("An older version is made the current one again", "[savesync][engine]") {
    NEEDS_SERVICE();
    const std::string title = NewTitle();
    Device thor{"Thor", title};
    Device z13{"Z13", title};
    thor.Save("before the mistake");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);
    thor.Save("after the mistake");
    REQUIRE(thor.engine.Prepare(thor.game).kind == Kind::Sent);

    // Not while the game is prepared or running.
    CHECK(thor.engine.Restore(thor.game, 1).kind == Kind::Failed);
    thor.engine.Release();

    thor.Save("played on after the mistake, not sent yet");
    const Result restored = thor.engine.Restore(thor.game, 1);
    CHECK(restored.kind == Kind::Fetched);
    CHECK(thor.Saved() == "before the mistake");
    TitleInfo history;
    REQUIRE(thor.engine.History(title, history).kind == Kind::InSync);
    // Two saves, the one put aside before restoring, and the restored one as a new version.
    REQUIRE(history.versions.size() == 4);
    CHECK(history.versions[0].kind == "restore");
    CHECK(history.versions[1].kind == "side");
    CHECK(history.head->n == 4);
    CHECK(z13.engine.Prepare(z13.game).kind == Kind::Fetched);
    CHECK(z13.Saved() == "before the mistake");
    CHECK(thor.engine.Restore(thor.game, 99).kind == Kind::Failed);
    CHECK(thor.Saved() == "before the mistake");
}

TEST_CASE("Reading statistics go up whole, one record for each install", "[savesync][engine]") {
    NEEDS_SERVICE();
    using Json = nlohmann::json;
    static std::mt19937_64 random{std::random_device{}()};
    const auto new_install = [] {
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(random()));
        return std::string{text};
    };
    const std::string thor = new_install();
    const std::string z13 = new_install();
    Device device{"thor", NewTitle()};
    Remote remote{ParseAddresses(Env("SAVESYNC_TEST_ADDRESS")), Env("SAVESYNC_TEST_CODE"), "test"};
    const auto record_of = [&](const std::string& install) -> Json {
        std::string text;
        REQUIRE_FALSE(remote.Stats(text));
        const Json all = Json::parse(text, nullptr, false);
        REQUIRE(all.is_object());
        Json found;
        int count = 0;
        for (const Json& record : all["devices"]) {
            if (record.value("id", std::string{}) == install) {
                found = record;
                ++count;
            }
        }
        REQUIRE(count <= 1);
        return found;
    };
    const Json first = {{"device", "Thor"},
                        {"app", "eden-reader"},
                        {"games", {{"0100000000000001", "作り物のゲーム"}}},
                        {"days",
                         {{{"day", "2026-10-03"},
                           {"game", "0100000000000001"},
                           {"active_ms", 600000},
                           {"chars", 1234}}}}};
    device.engine.SendStats(thor, first.dump());
    REQUIRE(device.engine.Flush(std::chrono::seconds(10)));
    Json kept = record_of(thor);
    REQUIRE(kept.is_object());
    CHECK(kept["device"] == "Thor");
    CHECK(kept["games"]["0100000000000001"] == "作り物のゲーム");
    CHECK(kept["days"].size() == 1);
    CHECK(kept["days"][0]["chars"] == 1234);
    CHECK(kept["received"].get<std::int64_t>() > 1700000000000);

    // Sent again, it takes the place of what was there: the record is the whole of it.
    Json second = first;
    second["days"].push_back({{"day", "2026-10-04"}, {"game", "0100000000000001"}, {"chars", 50}});
    device.engine.SendStats(thor, second.dump());
    // Another install's record stands beside it. In capitals the ID is the same install.
    std::string capitals = z13;
    std::transform(capitals.begin(), capitals.end(), capitals.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    device.engine.SendStats(capitals, Json{{"device", "Z13"}, {"days", Json::array()}}.dump());
    REQUIRE(device.engine.Flush(std::chrono::seconds(10)));
    CHECK(record_of(thor)["days"].size() == 2);
    CHECK(record_of(z13)["device"] == "Z13");

    // What is no record is refused and leaves the one that is there.
    CHECK(remote.PutStats(thor, "not json").failure == Failure::Refused);
    CHECK(remote.PutStats(thor, R"({"device": "Thor"})").failure == Failure::Refused);
    CHECK(remote.PutStats("../../etc/passwd", first.dump()).failure == Failure::Refused);
    CHECK(remote.PutStats("0123", first.dump()).failure == Failure::Refused);
    CHECK(record_of(thor)["days"].size() == 2);

    // The code is asked for.
    Remote stranger{ParseAddresses(Env("SAVESYNC_TEST_ADDRESS")), "AAAA-BBBB-CCCC", "test"};
    CHECK(stranger.PutStats(thor, first.dump()).failure == Failure::Unauthorised);
    std::string text;
    CHECK(stranger.Stats(text).failure == Failure::Unauthorised);
}

TEST_CASE("Switched off, nothing is asked of the service", "[savesync][engine]") {
    Scratch scratch;
    Engine engine;
    Config config;
    config.enabled = false;
    config.addresses = "127.0.0.1:1";
    config.code = "AAAA-BBBB-CCCC";
    config.state_file = scratch.path / "notes.json";
    config.backup_dir = scratch.path / "backups";
    engine.Configure(config);
    Game game;
    game.title = NewTitle();
    game.folders = Folders(scratch.path / "nand");
    Write(game.folders[0].path / "save.dat", "x");
    CHECK_FALSE(engine.IsOn());
    CHECK(engine.Prepare(game).kind == Kind::Off);
    CHECK(engine.Resolve(game, true).kind == Kind::Off);
    std::atomic<std::uint64_t> commits{0};
    engine.Watch(game, [&] { return commits.load(); });
    ++commits;
    engine.Unwatch();
    engine.SyncAll({game});
    CHECK(engine.Flush(std::chrono::seconds(5)));
    CHECK_FALSE(std::filesystem::exists(config.state_file));
}
