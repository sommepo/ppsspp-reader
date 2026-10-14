// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "reader/save_sync/engine.h"
#include "reader/save_sync/remote.h"

using namespace SaveSync;

namespace {

State Here(const std::string& hash) {
    State state;
    state.hash = hash;
    state.files = hash.empty() ? 0 : 1;
    state.size = hash.size();
    return state;
}

/// What the service has: versions 1 to the number of hashes, the last one current.
TitleInfo Service(const std::vector<std::string>& hashes) {
    TitleInfo info;
    int n = 0;
    for (const std::string& hash : hashes) {
        Version version;
        version.n = ++n;
        version.hash = hash;
        info.versions.insert(info.versions.begin(), version);
    }
    if (!info.versions.empty()) {
        info.head = info.versions.front();
    }
    return info;
}

} // namespace

TEST_CASE("What is done with a save", "[savesync][decide]") {
    const std::optional<Record> no_note;

    SECTION("the service has nothing of the game") {
        CHECK(Decide(Here(""), no_note, Service({})) == Step::Nothing);
        CHECK(Decide(Here("a"), no_note, Service({})) == Step::Upload);
        // Also with a note from a service that was set up anew since.
        CHECK(Decide(Here("a"), Record{4, "a"}, Service({})) == Step::Upload);
    }

    SECTION("this device has nothing of the game") {
        CHECK(Decide(Here(""), no_note, Service({"a"})) == Step::Download);
        // Deleted here: it comes back rather than being deleted everywhere.
        CHECK(Decide(Here(""), Record{1, "a"}, Service({"a"})) == Step::Download);
    }

    SECTION("the same on both sides") {
        CHECK(Decide(Here("b"), no_note, Service({"a", "b"})) == Step::Nothing);
        CHECK(Decide(Here("b"), Record{1, "a"}, Service({"a", "b"})) == Step::Nothing);
    }

    SECTION("only the other device played") {
        CHECK(Decide(Here("a"), Record{1, "a"}, Service({"a", "b"})) == Step::Download);
        CHECK(Decide(Here("a"), Record{1, "a"}, Service({"a", "b", "c"})) == Step::Download);
    }

    SECTION("only this device played") {
        CHECK(Decide(Here("b"), Record{1, "a"}, Service({"a"})) == Step::Upload);
        CHECK(Decide(Here("c"), Record{2, "b"}, Service({"a", "b"})) == Step::Upload);
    }

    SECTION("both played") {
        CHECK(Decide(Here("x"), Record{1, "a"}, Service({"a", "b"})) == Step::Ask);
    }

    SECTION("a device that is new to the service, with a save of its own") {
        CHECK(Decide(Here("x"), no_note, Service({"a"})) == Step::Ask);
    }

    SECTION("a save the service has as an older version needs no question") {
        // The note is gone (the app was set up anew), the save is version 1 as it was.
        CHECK(Decide(Here("a"), no_note, Service({"a", "b"})) == Step::Download);
        // An older version put back by hand and played on is this device's newer save when
        // nobody else saved since,
        CHECK(Decide(Here("a"), Record{2, "b"}, Service({"a", "b"})) == Step::Upload);
        // and a known older one when somebody did.
        CHECK(Decide(Here("a"), Record{2, "b"}, Service({"a", "b", "c"})) == Step::Download);
    }

    SECTION("a note that does not fit what the service has") {
        // Version 1 here says the note, and version 1 is current, but it is another save.
        CHECK(Decide(Here("x"), Record{1, "x"}, Service({"a"})) == Step::Ask);
    }
}

TEST_CASE("Addresses as they are typed", "[savesync][remote]") {
    const auto host = [](std::string_view text) {
        const auto address = ParseAddress(text);
        return address ? address->Text() : std::string{"-"};
    };
    CHECK(host("home.example.ts.net") == "home.example.ts.net:8642");
    CHECK(host("192.168.1.50:8700") == "192.168.1.50:8700");
    CHECK(host(" http://home.example.ts.net:8642/ ") == "home.example.ts.net:8642");
    CHECK(host("HTTP://host") == "host:8642");
    CHECK(host("[fd7a:115c::1]:9000") == "fd7a:115c::1:9000");
    CHECK(host("[fd7a:115c::1]") == "fd7a:115c::1:8642");
    CHECK(host("fd7a:115c::1") == "fd7a:115c::1:8642");
    CHECK(host("") == "-");
    CHECK(host("host:") == "-");
    CHECK(host("host:0") == "-");
    CHECK(host("host:70000") == "-");
    CHECK(host("host:12x") == "-");
    CHECK(host("host/path") == "-");
    CHECK(host("user@host") == "-");

    const auto several = ParseAddresses("home.example.ts.net, 192.168.1.50:8642;  bad:port\n"
                                        "home.example.ts.net:8642 other");
    REQUIRE(several.size() == 3);
    CHECK(several[0].Text() == "home.example.ts.net:8642");
    CHECK(several[1].Text() == "192.168.1.50:8642");
    CHECK(several[2].Text() == "other:8642");
    CHECK(ParseAddresses("  ").empty());

    CHECK(TitleText(0x0100AAAABBBBC000ULL) == "0100AAAABBBBC000");
    CHECK(TitleText(0x1) == "0000000000000001");
}
