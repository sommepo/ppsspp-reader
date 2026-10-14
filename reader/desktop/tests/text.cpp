// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/japanese_text.h"
#include "reader/desktop/tests/test_util.h"
#include "reader/desktop/text_line.h"

using namespace ReaderApp;

TEST_CASE("TextLine: the engine's event is parsed", "[reader][text]") {
    const QByteArray json = Q(uR"({"seq":7,"frame":120,"audio":4800,"channel":"dialogue",
        "speaker":"亜双義","text":"君の事は\n忘れない","ruby":[[0,1,"きみ"],[2,1,"こと"],[40,2,"x"]],
        "id":"s1:12","meta":{"voice":"v_012","n":3},"window":"main"})")
                                .toUtf8();
    const auto line = TextLine::FromJson(json, 1000);
    REQUIRE(line.has_value());
    REQUIRE(line->sequence == 7);
    REQUIRE(line->frame == 120);
    REQUIRE(line->audio_position == 4800);
    REQUIRE(line->speaker == Q(u"亜双義"));
    REQUIRE(line->text == Q(u"君の事は\n忘れない"));
    // The span that runs past the text is dropped.
    REQUIRE(line->ruby == QList<RubySpan>{{0, 1, Q(u"きみ")}, {2, 1, Q(u"こと")}});
    REQUIRE(line->source_id == QStringLiteral("s1:12"));
    REQUIRE(line->meta.value(QStringLiteral("voice")) == QStringLiteral("v_012"));
    REQUIRE(line->meta.value(QStringLiteral("n")) == QStringLiteral("3"));
    REQUIRE(line->timestamp == 1000);
    REQUIRE(line->window == QStringLiteral("main"));
    REQUIRE_FALSE(line->IsChoice());

    REQUIRE_FALSE(TextLine::FromJson("{\"text\":\"\"}", 0).has_value());
    REQUIRE_FALSE(TextLine::FromJson("nonsense", 0).has_value());
    const auto choice = TextLine::FromJson(Q(uR"({"text":"はい","channel":"choice"})").toUtf8(), 5);
    REQUIRE(choice->IsChoice());
    const auto bare = TextLine::FromJson(Q(uR"({"text":"はい"})").toUtf8(), 5);
    REQUIRE(bare->channel == QStringLiteral("dialogue"));
}

TEST_CASE("TextLine: furigana in Anki's syntax and as ruby", "[reader][text]") {
    TextLine line;
    line.text = Q(u"君の事は\n<忘>れない");
    line.ruby = {{0, 1, Q(u"きみ")}, {2, 1, Q(u"こと")}};
    REQUIRE(line.ToAnkiFurigana() == Q(u"君[きみ]の 事[こと]は\n<忘>れない"));
    REQUIRE(line.ToRubyHtml() ==
            Q(u"<ruby>君<rt>きみ</rt></ruby>の<ruby>事<rt>こと</rt></ruby>は<br>&lt;忘&gt;れない"));
    // Offsets count code points: a character outside the basic plane is one.
    TextLine rare;
    rare.text = Q(u"𠮷野家で");
    rare.ruby = {{0, 2, Q(u"よしの")}};
    REQUIRE(rare.ToAnkiFurigana() == Q(u"𠮷野[よしの]家で"));
    REQUIRE(CodePointCount(rare.text) == 4);
    REQUIRE(OffsetByCodePoints(rare.text, 0, 1) == 2);
    REQUIRE(OffsetByCodePoints(rare.text, 0, 99) == rare.text.size());
}

TEST_CASE("TextLine: JSON round trip keeps ruby and meta", "[reader][text]") {
    TextLine line;
    line.sequence = 3;
    line.channel = QStringLiteral("dialogue");
    line.text = Q(u"漢字");
    line.ruby = {{0, 2, Q(u"かんじ")}};
    line.meta.insert(QStringLiteral("k"), QStringLiteral("v"));
    line.timestamp = 42;
    const auto back =
        TextLine::FromJson(QJsonDocument(line.ToJson()).toJson(QJsonDocument::Compact), 0);
    REQUIRE(back.has_value());
    REQUIRE(back->text == line.text);
    REQUIRE(back->ruby == line.ruby);
    REQUIRE(back->meta == line.meta);
    REQUIRE(back->timestamp == 42);
}

TEST_CASE("JapaneseText: scripts and conversions", "[reader][text]") {
    using namespace JapaneseText;
    REQUIRE(KatakanaToHiragana(Q(u"タベル・ー漢字")) == Q(u"たべる・ー漢字"));
    REQUIRE(HiraganaToKatakana(Q(u"たべる")) == Q(u"タベル"));
    REQUIRE(FullWidthToAscii(Q(u"ＡＢＣ１２３ｘ！")) == QStringLiteral("ABC123x!"));
    REQUIRE(IsKana(u'ー'));
    REQUIRE(IsKanji(U'々'));
    REQUIRE(IsKanji(0x20BB7)); // 𠮷
    REQUIRE(IsTermChar(U'a'));
    REQUIRE(IsTermChar(U'７'));
    REQUIRE_FALSE(IsTermChar(U'。'));
    REQUIRE_FALSE(IsTermChar(U'　'));
}

TEST_CASE("JapaneseText: readings sit over the kanji only", "[reader][text]") {
    using namespace JapaneseText;
    REQUIRE(AnkiFurigana(Q(u"食べ物"), Q(u"たべもの")) == Q(u"食[た]べ 物[もの]"));
    REQUIRE(AnkiFurigana(Q(u"お茶"), Q(u"おちゃ")) == Q(u"お 茶[ちゃ]"));
    REQUIRE(AnkiFurigana(Q(u"かな"), Q(u"かな")) == Q(u"かな"));
    REQUIRE(AnkiFurigana(Q(u"コーヒー"), QString()) == Q(u"コーヒー"));
    // Kana that cannot be found in the reading: the whole reading over the whole word.
    REQUIRE(AnkiFurigana(Q(u"今日は"), Q(u"きょうも")) == Q(u"今日は[きょうも]"));
    REQUIRE(RubyHtml(Q(u"取り引き"), Q(u"とりひき")) ==
            Q(u"<ruby>取<rt>と</rt></ruby>り<ruby>引<rt>ひ</rt></ruby>き"));
    REQUIRE(DistributeFurigana(Q(u"東京"), Q(u"とうきょう")) ==
            QList<FuriganaSegment>{{Q(u"東京"), Q(u"とうきょう")}});
}
