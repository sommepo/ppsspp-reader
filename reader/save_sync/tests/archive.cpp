// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "reader/save_sync/archive.h"
#include "reader/save_sync/tests/test_util.h"

using namespace SaveSync;
using namespace SaveSyncTest;

TEST_CASE("SHA-256 is the real one", "[savesync][archive]") {
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("A save's state goes by names, sizes and contents", "[savesync][archive]") {
    Scratch scratch;
    const auto folders = Folders(scratch.path);

    // No folders at all, and folders without files: the same empty state.
    const State nothing = StateOf(folders);
    CHECK(nothing.Empty());
    std::filesystem::create_directories(folders[0].path / "empty");
    CHECK(StateOf(folders).hash == nothing.hash);
    CHECK(StateOf(folders).Empty());

    Write(folders[0].path / "save.dat", "progress");
    Write(folders[0].path / "sub" / "b.bin", std::string("\0\1\2", 3));
    const State first = StateOf(folders);
    CHECK(first.files == 2);
    CHECK(first.size == 11);
    CHECK(first.newest_ms > 1'600'000'000'000);
    CHECK(first.hash != nothing.hash);
    // What the service computes for the same files (tools/reader/savesync/server.py):
    // sha256 over "name\0size\0" + sha256(content) for each file in the order of the names.
    CHECK(first.hash == "f3c48799f7ff359237fabacfe04ab8388850ae396a5a7cfe4cd7a88b4229f676");

    // Written again with the same content: the same state. Another content, name or place:
    // another state.
    Write(folders[0].path / "save.dat", "progress");
    CHECK(StateOf(folders).hash == first.hash);
    Write(folders[0].path / "save.dat", "progresS");
    CHECK(StateOf(folders).hash != first.hash);
    Write(folders[0].path / "save.dat", "progress");
    std::filesystem::rename(folders[0].path / "save.dat", folders[0].path / "save2.dat");
    CHECK(StateOf(folders).hash != first.hash);
    std::filesystem::rename(folders[0].path / "save2.dat", folders[0].path / "save.dat");
    CHECK(StateOf(folders).hash == first.hash);
    // The same file in the other folder is another save.
    std::filesystem::create_directories(folders[1].path);
    std::filesystem::rename(folders[0].path / "save.dat", folders[1].path / "save.dat");
    CHECK(StateOf(folders).hash != first.hash);
}

TEST_CASE("A packed save comes back as it was", "[savesync][archive]") {
    Scratch scratch;
    const auto folders = Folders(scratch.path / "here");
    std::string large;
    for (int i = 0; i < 200000; ++i) {
        large.push_back(static_cast<char>((i * 7 + i / 255) & 0xff));
    }
    Write(folders[0].path / "save.dat", std::string(50000, 'x'));
    Write(folders[0].path / "large.bin", large);
    Write(folders[0].path / "empty.bin", "");
    Write(folders[0].path / "deep" / "er" / "file", "a");
    Write(folders[0].path / u8"セーブ" / u8"データ.bin", "japanese names");
    std::filesystem::create_directories(folders[0].path / "nothing in here");
    Write(folders[1].path / "device.dat", "shared by all profiles");

    const Snapshot snapshot = Pack(folders);
    CHECK(snapshot.state.files == 6);
    CHECK(snapshot.state.hash == StateOf(folders).hash);
    // Compressed: the 50,000 equal letters take next to nothing.
    CHECK(snapshot.zip.size() < 230000);
    const State packed = StateOfArchive(snapshot.zip);
    CHECK(packed.hash == snapshot.state.hash);
    CHECK(packed.files == 6);
    CHECK(packed.size == snapshot.state.size);

    // Onto a device that has nothing of the game.
    const auto there = Folders(scratch.path / "there");
    Unpack(snapshot.zip, there, scratch.path / "backup-1");
    CHECK(Tree(there[0].path) == Tree(folders[0].path));
    CHECK(Tree(there[1].path) == Tree(folders[1].path));
    CHECK(std::filesystem::is_directory(there[0].path / "nothing in here"));
    CHECK(StateOf(there).hash == snapshot.state.hash);
    CHECK_FALSE(std::filesystem::exists(scratch.path / "backup-1"));
}

TEST_CASE("The save that is replaced is moved aside whole", "[savesync][archive]") {
    Scratch scratch;
    const auto source = Folders(scratch.path / "source");
    Write(source[0].path / "save.dat", "the new one");
    const Snapshot snapshot = Pack(source);

    const auto folders = Folders(scratch.path / "here");
    Write(folders[0].path / "save.dat", "the old one");
    Write(folders[0].path / "only-old.dat", "gone from the new save");
    Write(folders[1].path / "device.dat", "the new save has no device part");
    const auto old_user = Tree(folders[0].path);
    const auto old_device = Tree(folders[1].path);

    const auto backup = scratch.path / "backups" / "one";
    Unpack(snapshot.zip, folders, backup);
    CHECK(Tree(folders[0].path) ==
          std::map<std::string, std::string>{{"save.dat", "the new one"}});
    // A part the new save does not have is not there afterwards.
    CHECK_FALSE(std::filesystem::exists(folders[1].path));
    CHECK(Tree(backup / "user") == old_user);
    CHECK(Tree(backup / "device") == old_device);
    // Nothing is left beside the save folders.
    for (const auto& entry : std::filesystem::directory_iterator{scratch.path / "here"}) {
        CHECK(entry.path().filename() == "user-save");
    }
}

TEST_CASE("What is not a save archive leaves the save alone", "[savesync][archive]") {
    Scratch scratch;
    const auto folders = Folders(scratch.path / "here");
    Write(folders[0].path / "save.dat", "mine");
    const auto before = Tree(scratch.path / "here");

    const auto source = Folders(scratch.path / "source");
    Write(source[0].path / "save.dat", std::string(5000, 'n'));
    const Snapshot good = Pack(source);

    std::vector<std::string> bad;
    bad.push_back("");
    bad.push_back("not an archive at all, but long enough to be looked at");
    bad.push_back(good.zip.substr(0, good.zip.size() - 10));
    bad.push_back(good.zip.substr(0, good.zip.size() / 2));
    // A byte of the file's packed content changed (it follows the file's name).
    std::string flipped = good.zip;
    const std::size_t content = flipped.find("user/save.dat") + std::string("user/save.dat").size();
    flipped[content + 2] = static_cast<char>(flipped[content + 2] ^ 0x55);
    bad.push_back(flipped);
    for (const std::string& zip : bad) {
        CHECK_THROWS_AS(StateOfArchive(zip), ArchiveError);
        CHECK_THROWS_AS(Unpack(zip, folders, scratch.path / "backup"), ArchiveError);
        CHECK(Tree(scratch.path / "here") == before);
    }

    // A part the app has no folder for.
    const std::vector<Folder> other{{"elsewhere", scratch.path / "source" / "user-save"}};
    const Snapshot unknown = Pack(other);
    CHECK_THROWS_AS(Unpack(unknown.zip, folders, scratch.path / "backup"), ArchiveError);
    CHECK(Tree(scratch.path / "here") == before);
    CHECK_FALSE(std::filesystem::exists(scratch.path / "backup"));
}

TEST_CASE("Names that would leave the save folder are refused", "[savesync][archive]") {
    // An archive as this code writes it, with the name changed by hand to one of the same
    // length (in the file's header and in the directory at the end).
    Scratch scratch;
    const std::vector<Folder> source{{"user", scratch.path / "source"}};
    Write(source[0].path / "aa" / "bb", "x");
    const std::string good = Pack(source).zip;
    REQUIRE(StateOfArchive(good).files == 1);
    for (const std::string name : {"user/../bb", "user/./ bb", "user//a/bb", "user\\aa\\bb"}) {
        std::string zip = good;
        REQUIRE(name.size() == std::string("user/aa/bb").size());
        std::size_t at = 0;
        int replaced = 0;
        while ((at = zip.find("user/aa/bb", at)) != std::string::npos) {
            zip.replace(at, name.size(), name);
            ++replaced;
        }
        REQUIRE(replaced == 2);
        CHECK_THROWS_AS(StateOfArchive(zip), ArchiveError);
    }
}

TEST_CASE("Old backups are removed, the newest kept", "[savesync][archive]") {
    Scratch scratch;
    for (const char* name : {"20261001-100000", "20261002-100000", "20261003-100000",
                             "20261004-100000"}) {
        Write(scratch.path / name / "user" / "save.dat", name);
    }
    PruneBackups(scratch.path, 2);
    CHECK_FALSE(std::filesystem::exists(scratch.path / "20261001-100000"));
    CHECK_FALSE(std::filesystem::exists(scratch.path / "20261002-100000"));
    CHECK(std::filesystem::exists(scratch.path / "20261003-100000"));
    CHECK(std::filesystem::exists(scratch.path / "20261004-100000"));
    PruneBackups(scratch.path / "not there", 2);
}
