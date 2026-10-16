// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "reader/core/crt.h"
#include "reader/core/crt_frame.h"

namespace Crt = Reader::Crt;
using Catch::Approx;

namespace {
// The values are the program's own: every test begins from the beginning.
void Fresh() {
    Crt::Reset();
    Crt::Set("enabled", 0);
    Crt::SetNativeLines(480);
}

// The soft look and the console's own number of lines: what the tests of the shader's
// numbers were written against.
void Soft() {
    Fresh();
    Crt::ApplyPreset("warm");
}
} // namespace

TEST_CASE("CRT: the table has one row a key, each within its own range", "[crt]") {
    std::set<std::string> keys;
    for (const Crt::Param& param : Crt::Params()) {
        REQUIRE(keys.insert(param.key).second);
        REQUIRE(param.low < param.high);
        REQUIRE(param.step > 0);
        REQUIRE(param.initial >= param.low);
        REQUIRE(param.initial <= param.high);
        if (param.kind == Crt::Kind::Choice) {
            REQUIRE(param.choices != nullptr);
            // The value it starts with is one of its choices.
            const std::string wanted = std::to_string(static_cast<int>(param.initial)) + "=";
            const std::string choices = std::string("|") + param.choices;
            REQUIRE(choices.find("|" + wanted) != std::string::npos);
        }
    }
    REQUIRE(std::strcmp(Crt::Params().front().key, "enabled") == 0);
}

TEST_CASE("CRT: switched on and off, and a value stays within its range", "[crt]") {
    Fresh();
    REQUIRE_FALSE(Crt::Enabled());
    Crt::Set("enabled", 1);
    REQUIRE(Crt::Enabled());
    Crt::Set("warp", 7);
    REQUIRE(Crt::Get("warp") == Approx(1.0f));
    Crt::Set("warmth", -3);
    REQUIRE(Crt::Get("warmth") == Approx(-1.0f));
    const unsigned before = Crt::Revision();
    Crt::Set("no such thing", 1);
    Crt::Set("warp", 1); // as it is
    REQUIRE(Crt::Revision() == before);
    Crt::Set("warp", 0.5f);
    REQUIRE(Crt::Revision() != before);
}

TEST_CASE("CRT: the kept text gives the values back", "[crt]") {
    Fresh();
    Crt::Set("enabled", 1);
    Crt::Set("warmth", -0.35f);
    Crt::Set("lines", 288);
    Crt::Set("mask", 3);
    const std::string kept = Crt::Serialize();
    REQUIRE(kept.find("warmth=-0.350") != std::string::npos);
    Fresh();
    Crt::Deserialize(kept);
    REQUIRE(Crt::Enabled());
    REQUIRE(Crt::Get("warmth") == Approx(-0.35f));
    REQUIRE(Crt::Get("lines") == Approx(288.0f));
    REQUIRE(Crt::Get("mask") == Approx(3.0f));
    REQUIRE(Crt::Serialize() == kept);
}

TEST_CASE("CRT: text from elsewhere does no harm", "[crt]") {
    Fresh();
    const float glow = Crt::Get("glow");
    const float bloom = Crt::Get("bloom");
    const float grain = Crt::Get("grain");
    Crt::Deserialize("warp=0.3;;nonsense;glow=abc;=3;unknown=1;halation=0.2;bloom=1e9;grain=,5");
    REQUIRE(Crt::Get("warp") == Approx(0.3f));
    REQUIRE(Crt::Get("halation") == Approx(0.2f));
    REQUIRE(Crt::Get("glow") == Approx(glow));   // not a number: as it was
    REQUIRE(Crt::Get("bloom") == Approx(bloom)); // not written as the kept text writes one
    REQUIRE(Crt::Get("grain") == Approx(grain));
    Crt::Deserialize("");
    REQUIRE(Crt::Get("warp") == Approx(0.3f));
}

TEST_CASE("CRT: a ready-made look is known again, and is the user's own once changed", "[crt]") {
    Fresh();
    // The values the table starts with are the first look's.
    REQUIRE(Crt::CurrentPreset() == Crt::Presets().front().key);
    for (const Crt::Preset& preset : Crt::Presets()) {
        Crt::Set("enabled", 1);
        REQUIRE(Crt::ApplyPreset(preset.key));
        REQUIRE(Crt::CurrentPreset() == preset.key);
        REQUIRE(Crt::Enabled()); // a look does not switch the picture off
    }
    REQUIRE_FALSE(Crt::ApplyPreset("no such look"));
    Crt::ApplyPreset("trinitron");
    Crt::Set("warp", 0.9f);
    REQUIRE(Crt::CurrentPreset().empty());
    // A look sets everything: nothing of the one before is left.
    Crt::ApplyPreset("television");
    Crt::ApplyPreset("warm");
    REQUIRE(Crt::Get("flicker") == Approx(0.0f));
    REQUIRE(Crt::Get("glass") == Approx(0.0f));
    // The first look is the table's own values, the number of lines among them.
    Crt::ApplyPreset(Crt::Presets().front().key);
    for (const Crt::Param& param : Crt::Params()) {
        if (std::strcmp(param.key, "enabled") != 0) {
            REQUIRE(Crt::Get(param.key) == Approx(param.initial));
        }
    }
}

TEST_CASE("CRT: what the shader is handed", "[crt]") {
    Soft();
    Crt::Picture picture;
    picture.source_width = 640;
    picture.source_height = 480;
    picture.output_width = 1920;
    picture.output_height = 1440;
    float u[Crt::UniformFloats];
    Crt::Pack(picture, u);
    REQUIRE(u[2] == Approx(1.0f)); // the picture is the whole source, upright
    REQUIRE(u[5] == Approx(1.0f));
    REQUIRE(u[6] == Approx(640.0f));
    REQUIRE(u[9] == Approx(1440.0f));
    // 480 lines at three screen pixels each; the source has a row a line, so shows its rows.
    REQUIRE(std::floor(u[10]) == Approx(480.0f));
    REQUIRE(u[10] - std::floor(u[10]) == Approx(0.0f).margin(0.001));
    REQUIRE(u[11] == Approx(640.0f));
    // The slot mask, one pixel large on a screen of this size.
    REQUIRE(std::floor(u[13]) == Approx(2.0f));
    REQUIRE(std::floor((u[13] - std::floor(u[13])) * 10 + 0.5f) == Approx(1.0f));

    SECTION("a screen too coarse for the console's lines gets half of them") {
        picture.output_width = 1440;
        picture.output_height = 1080;
        Crt::Pack(picture, u);
        REQUIRE(std::floor(u[10]) == Approx(240.0f));
        // Two source rows a line now: the source keeps its own detail under the lines.
        REQUIRE(u[10] - std::floor(u[10]) == Approx(0.99f).margin(0.001));
    }
    SECTION("lines the screen cannot show fade out") {
        picture.output_width = 640;
        picture.output_height = 480;
        Crt::Pack(picture, u);
        REQUIRE(std::floor(u[10]) == Approx(240.0f));
        REQUIRE(u[14] == Approx(0.0f)); // two pixels a line: no scan lines
    }
    SECTION("the user's number of lines is taken as it is") {
        Crt::Set("lines", 288);
        Crt::Pack(picture, u);
        REQUIRE(std::floor(u[10]) == Approx(288.0f));
    }
    SECTION("a finer source is measured in the console's pixels, not its own") {
        picture.source_width = 2560;
        picture.source_height = 1920;
        Crt::Pack(picture, u);
        REQUIRE(u[11] == Approx(640.0f));
    }
    SECTION("the phosphor grows with the screen") {
        picture.output_width = 2880;
        picture.output_height = 2160;
        Crt::Pack(picture, u);
        REQUIRE(std::floor((u[13] - std::floor(u[13])) * 10 + 0.5f) == Approx(2.0f));
        Crt::Set("mask_size", 3);
        Crt::Pack(picture, u);
        REQUIRE(std::floor((u[13] - std::floor(u[13])) * 10 + 0.5f) == Approx(3.0f));
    }
}

TEST_CASE("CRT: the glass bends a place and brings it back", "[crt]") {
    Fresh();
    float x = 0.9f;
    float y = 0.85f;
    Crt::ToScreen(x, y); // off: nothing bends
    REQUIRE(x == Approx(0.9f));
    REQUIRE(y == Approx(0.85f));

    Crt::Set("enabled", 1);
    Crt::Set("warp", 1);
    Crt::ToScreen(x, y);
    // What is near a corner is seen nearer the middle.
    REQUIRE(x < 0.9f);
    REQUIRE(y < 0.85f);
    Crt::FromScreen(x, y);
    REQUIRE(x == Approx(0.9f).margin(0.0005));
    REQUIRE(y == Approx(0.85f).margin(0.0005));

    float middle_x = 0.5f;
    float middle_y = 0.5f;
    Crt::ToScreen(middle_x, middle_y);
    REQUIRE(middle_x == Approx(0.5f));
    REQUIRE(middle_y == Approx(0.5f));

    Crt::Set("warp", 0);
    x = 0.9f;
    Crt::ToScreen(x, y);
    REQUIRE(x == Approx(0.9f));
}

TEST_CASE("CRT: the shader's text is there for both languages", "[crt]") {
    const std::string body = Crt::ShaderBody();
    REQUIRE(body.find("vec3 ReaderCrt(vec2 p)") != std::string::npos);
    REQUIRE(body.find("CRT_U(7)") != std::string::npos);
    REQUIRE(body.find("CRT_U(8)") == std::string::npos); // eight vectors, no more
    REQUIRE(std::string(Crt::HlslDefines()).find("float3") != std::string::npos);
}

TEST_CASE("CRT: the tube's frame leaves the glass clear and is the same for every app", "[crt]") {
    Reader::Crt::Set("enabled", 1.f);
    Reader::Crt::Set("frame", 1.f); // charcoal
    Reader::Crt::Set("frame_wear", 0.5f);
    REQUIRE(Reader::Crt::Frame::Wanted());
    // A 4:3 picture in the middle of a 16:9 screen, as a PS2 game on a handheld.
    const int W = 320, H = 180;
    const float left = 40, top = 0, pw = 240, ph = 180;
    const auto straight = Reader::Crt::Frame::Draw(W, H, left, top, pw, ph, false);
    const auto pre = Reader::Crt::Frame::Draw(W, H, left, top, pw, ph, true);
    REQUIRE(straight.size() == static_cast<std::size_t>(W * H));
    const auto at = [&](const std::vector<std::uint32_t>& p, int x, int y) { return p[y * W + x]; };
    // The middle of the glass: nothing over the picture.
    CHECK((at(straight, W / 2, H / 2) >> 24) == 0);
    // Beside the picture: the case, solid.
    CHECK((at(straight, 5, H / 2) >> 24) == 255);
    CHECK((at(straight, W - 5, H / 2) >> 24) == 255);
    // Where the case is solid, straight and premultiplied colours are the same.
    CHECK(at(straight, 5, H / 2) == at(pre, 5, H / 2));
    // A premultiplied colour is never above its alpha.
    for (const std::uint32_t c : pre) {
        const std::uint32_t a = c >> 24;
        CHECK(((c >> 16) & 0xff) <= a);
    }
    Reader::Crt::Set("frame", 0.f); // none
    CHECK_FALSE(Reader::Crt::Frame::Wanted());
    CHECK(Reader::Crt::Frame::Draw(W, H, left, top, pw, ph, false).empty());
}
