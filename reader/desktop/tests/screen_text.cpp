// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/screen_text.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

PresentedLine Presented(const QString& text, qint64 sequence, qint64 backlog_id,
                        const QString& window = QStringLiteral("main")) {
    PresentedLine presented;
    presented.line.text = text;
    presented.line.sequence = sequence;
    presented.line.window = window;
    presented.backlog_id = backlog_id;
    return presented;
}

/// Two rows of three characters, 40 units wide and tall, in a 1280x720 picture.
ScreenLayout TwoRows(qint64 sequence) {
    ScreenLayout layout;
    layout.window = QStringLiteral("main");
    layout.sequence = sequence;
    layout.box = QRectF(80, 480, 1120, 200);
    for (int i = 0; i < 3; ++i) {
        layout.cells.append(QRectF(100 + i * 40, 500, 40, 40));
    }
    layout.cells.append(std::nullopt); // the line break
    for (int i = 0; i < 3; ++i) {
        layout.cells.append(QRectF(100 + i * 40, 560, 40, 40));
    }
    return layout;
}

} // namespace

TEST_CASE("ScreenLayout: the engine's layout is parsed", "[reader][screen]") {
    const auto layout = ScreenLayout::FromJson(
        R"({"window":"main","seq":4,"space":[1920,1080],"box":[10,20,300,40],
            "glyphs":[[1,2,3,4],null,[5,6,7,-1],[1,2,3]]})");
    REQUIRE(layout.has_value());
    REQUIRE(layout->window == QStringLiteral("main"));
    REQUIRE(layout->sequence == 4);
    REQUIRE(layout->space_width == 1920.0f);
    REQUIRE(layout->space_height == 1080.0f);
    REQUIRE(layout->box == QRectF(10, 20, 300, 40));
    REQUIRE(layout->cells.size() == 4);
    REQUIRE(layout->cells.at(0) == QRectF(1, 2, 3, 4));
    REQUIRE_FALSE(layout->cells.at(1).has_value());
    REQUIRE_FALSE(layout->cells.at(2).has_value());
    REQUIRE_FALSE(layout->cells.at(3).has_value());

    const auto closed = ScreenLayout::FromJson(R"({"window":"main","seq":5,"closed":true})");
    REQUIRE(closed.has_value());
    REQUIRE(closed->closed);
    REQUIRE_FALSE(ScreenLayout::FromJson(R"({"seq":5,"glyphs":[]})").has_value());
    REQUIRE_FALSE(ScreenLayout::FromJson(R"({"window":"main","seq":5})").has_value());
    // No size given: the console's own.
    REQUIRE(ScreenLayout::FromJson(R"({"window":"w","seq":1,"glyphs":[]})")->space_width ==
            1280.0f);
}

TEST_CASE("ScreenTexts: a line and its layout meet in either order", "[reader][screen]") {
    ScreenTexts texts;
    // The line first.
    REQUIRE_FALSE(texts.AdoptLine(Presented(Q(u"あいう\nえおか"), 1, 10)));
    REQUIRE(texts.OnScreen().isEmpty());
    REQUIRE(texts.ApplyLayout(TwoRows(1)));
    REQUIRE(texts.OnScreen().size() == 1);
    REQUIRE(texts.PlacementOf(Presented(Q(u"あいう\nえおか"), 1, 10)).has_value());
    REQUIRE_FALSE(texts.PlacementOf(Presented(Q(u"x"), 1, 99)).has_value());

    // The layout first: it waits for its line.
    REQUIRE_FALSE(texts.ApplyLayout(TwoRows(2)));
    REQUIRE(texts.OnScreen().first().presented.backlog_id == 10);
    REQUIRE(texts.AdoptLine(Presented(Q(u"きくけ\nこさし"), 2, 11)));
    REQUIRE(texts.OnScreen().size() == 1);
    REQUIRE(texts.OnScreen().first().presented.backlog_id == 11);

    // A new line takes the old placement away until its own layout comes.
    REQUIRE(texts.AdoptLine(Presented(Q(u"すせそ"), 3, 12)));
    REQUIRE(texts.OnScreen().isEmpty());
    // A layout for text that has been replaced since changes nothing.
    REQUIRE_FALSE(texts.ApplyLayout(TwoRows(2)));
    REQUIRE(texts.ApplyLayout(TwoRows(3)));

    ScreenLayout closed;
    closed.window = QStringLiteral("main");
    closed.closed = true;
    REQUIRE(texts.ApplyLayout(closed));
    REQUIRE(texts.OnScreen().isEmpty());
    REQUIRE_FALSE(texts.ApplyLayout(closed));

    REQUIRE_FALSE(texts.AdoptLine(Presented(Q(u"窓なし"), 4, 13, QString())));
    REQUIRE(texts.ApplyLayout(TwoRows(3)));
    REQUIRE(texts.Clear());
    REQUIRE_FALSE(texts.Clear());
}

TEST_CASE("PlacedText: characters are found where the picture is drawn", "[reader][screen]") {
    const PresentedLine presented = Presented(Q(u"あいう\nえおか"), 1, 10);
    // The picture at half size, 100 pixels in from the left and 50 down.
    const QRectF area(100, 50, 640, 360);
    const auto placed = PlacedText::Place(presented, TwoRows(1), area);
    REQUIRE(placed.has_value());
    REQUIRE(placed->characters.size() == 6);
    // The first character: (100,500) 40x40 in the game's units.
    REQUIRE(placed->characters.at(0).drawn == QRectF(150, 300, 20, 20));
    REQUIRE(placed->characters.at(0).offset == 0);
    REQUIRE(placed->characters.at(0).row == 0);
    // After the line break: its offset in the text is 4, its row the second.
    REQUIRE(placed->characters.at(3).offset == 4);
    REQUIRE(placed->characters.at(3).row == 1);
    // The box the profile reported, grown to hold the room around the characters.
    REQUIRE(placed->box == QRectF(138, 288, 562, 102));

    const auto on = [&placed](qreal x, qreal y) { return placed->HitTest(QPointF(x, y)); };
    REQUIRE(on(160, 310).kind == PlacedText::HitKind::Character);
    REQUIRE(on(160, 310).glyph->offset == 0);
    REQUIRE(on(195, 310).glyph->offset == 2);
    // Just above the first row is where its furigana would be: still that character.
    REQUIRE(on(160, 292).glyph->offset == 0);
    // Left of a row's first character there is room for a pointer.
    REQUIRE(on(142, 310).glyph->offset == 0);
    // The gap between the rows belongs to the lower one.
    REQUIRE(on(160, 325).glyph->offset == 4);
    REQUIRE(on(180, 340).glyph->offset == 5);
    // In the box, on no character: taken, but nothing to look up.
    REQUIRE(on(600, 310).kind == PlacedText::HitKind::Box);
    REQUIRE(on(600, 310).glyph == nullptr);
    // Outside the box: the game's.
    REQUIRE(on(600, 100).kind == PlacedText::HitKind::Outside);
    REQUIRE(on(50, 310).kind == PlacedText::HitKind::Outside);
}

TEST_CASE("PlacedText: a word is marked row by row", "[reader][screen]") {
    const PresentedLine presented = Presented(Q(u"あいう\nえおか"), 1, 10);
    const auto placed = PlacedText::Place(presented, TwoRows(1), QRectF(0, 0, 1280, 720));
    REQUIRE(placed.has_value());
    // い to お: the end of the first row and the start of the second.
    const QList<QRectF> marks = placed->Marks(1, 5);
    REQUIRE(marks.size() == 2);
    REQUIRE(marks.at(0) == QRectF(140, 500, 80, 40));
    REQUIRE(marks.at(1) == QRectF(100, 560, 80, 40));
    REQUIRE(placed->Marks(3, 3).isEmpty()); // the line break is drawn nowhere
}

TEST_CASE("PlacedText: nothing is placed before anything is drawn", "[reader][screen]") {
    ScreenLayout layout = TwoRows(1);
    for (auto& cell : layout.cells) {
        cell.reset();
    }
    REQUIRE_FALSE(PlacedText::Place(Presented(Q(u"あいう\nえおか"), 1, 10), layout,
                                    QRectF(0, 0, 1280, 720))
                      .has_value());
    // Text outside the basic plane takes two units of the string and one cell.
    ScreenLayout rare;
    rare.window = QStringLiteral("main");
    rare.cells = {QRectF(0, 0, 10, 10), QRectF(10, 0, 10, 10)};
    const auto placed =
        PlacedText::Place(Presented(Q(u"𠮷野"), 1, 1), rare, QRectF(0, 0, 1280, 720));
    REQUIRE(placed->characters.at(1).offset == 2);
}
