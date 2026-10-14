// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/anki.h"
#include "reader/desktop/card_builder.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

/// A card about 食べた in a made-up line, with what two made-up dictionaries say of 食べる.
CardContext WordCard() {
    CardContext card;
    card.line.text = Q(u"昨日、<魚>を食べた。\nおいしかった");
    card.line.speaker = Q(u"成歩堂");
    card.line.ruby = {{0, 2, Q(u"きのう")}};
    card.game_title = Q(u"Test & Game");
    LookupResult word;
    word.expression = Q(u"食べる");
    word.reading = Q(u"たべる");
    word.matched_text = Q(u"食べた");
    word.reasons = {QStringLiteral("past")};
    TermEntry first;
    first.dictionary = QStringLiteral("Dict A");
    first.definition_tags = QStringLiteral("v1 vt");
    first.glossary_json = QStringLiteral("[\"to eat\",\"to live on\"]");
    TermEntry second;
    second.dictionary = QStringLiteral("Dict B");
    second.definition_tags = QStringLiteral("v1");
    second.glossary_json = Q(u"[\"食物を口に入れる\"]");
    word.entries = {first, second};
    word.frequencies = {Frequency{QStringLiteral("F1"), QStringLiteral("800"), 800.0, true},
                        Frequency{QStringLiteral("F2"), QStringLiteral("1500"), 1500.0, true}};
    word.pitches = {PitchAccent{QStringLiteral("P"), Q(u"たべる"), {2, 0}},
                    PitchAccent{QStringLiteral("P2"), Q(u"たべる"), {2}}};
    card.word = word;
    card.word_first = 7;
    card.word_last = 9;
    return card;
}

QString One(const QString& text, const CardContext& card) {
    return CardBuilder::Build({{QStringLiteral("f"), text}}, card).value(QStringLiteral("f"));
}

} // namespace

TEST_CASE("CardBuilder: markers of a word card", "[reader][cards]") {
    const CardContext card = WordCard();
    REQUIRE(One(QStringLiteral("{expression}"), card) == Q(u"食べる"));
    REQUIRE(One(QStringLiteral("{reading}"), card) == Q(u"たべる"));
    REQUIRE(One(QStringLiteral("{furigana}"), card) == Q(u"食[た]べる"));
    REQUIRE(One(QStringLiteral("{furigana-html}"), card) == Q(u"<ruby>食<rt>た</rt></ruby>べる"));
    REQUIRE(One(QStringLiteral("{sentence}"), card) ==
            Q(u"昨日、&lt;魚&gt;を<b>食べた</b>。<br>おいしかった"));
    REQUIRE(One(QStringLiteral("{sentence-plain}"), card) ==
            Q(u"昨日、&lt;魚&gt;を食べた。<br>おいしかった"));
    REQUIRE(One(QStringLiteral("{sentence-furigana}"), card) ==
            Q(u"昨日[きのう]、&lt;魚&gt;を食べた。<br>おいしかった"));
    REQUIRE(One(QStringLiteral("{speaker} / {source}"), card) == Q(u"成歩堂 / Test &amp; Game"));
    // The game's page, as {url} is the page's address in Yomitan; nothing without a game.
    REQUIRE(One(QStringLiteral("{url}"), card).isEmpty());
    CardContext addressed = card;
    addressed.game_url = SourceAddress::Of(QStringLiteral("ULJM05000"));
    REQUIRE(One(QStringLiteral("{url}"), addressed) ==
            QStringLiteral("https://www.jp.playstation.com/software/title/uljm05000.html"));
    REQUIRE(One(QStringLiteral("<a href=\"{url}\">{source}</a>"), addressed) ==
            Q(u"<a href=\"https://www.jp.playstation.com/software/title/uljm05000.html\">Test &amp; Game</a>"));
    REQUIRE(One(QStringLiteral("{frequencies}"), card) == QStringLiteral("F1: 800<br>F2: 1500"));
    REQUIRE(One(QStringLiteral("{frequency-rank}"), card) == QStringLiteral("1043"));
    REQUIRE(One(QStringLiteral("{pitch-positions}"), card) == QStringLiteral("2, 0"));
    REQUIRE(One(QStringLiteral("{part-of-speech}"), card) == QStringLiteral("v1, vt"));
    REQUIRE(One(QStringLiteral("{inflection}"), card) == QStringLiteral("past"));
    REQUIRE(One(QStringLiteral("{matched-text}"), card) == Q(u"食べた"));
    REQUIRE(One(QStringLiteral("{glossary-plain}"), card) ==
            Q(u"to eat; to live on<br>食物を口に入れる"));
}

TEST_CASE("CardBuilder: the glossary keeps Yomitan's structure", "[reader][cards]") {
    const CardContext card = WordCard();
    const QString all = One(QStringLiteral("{glossary}"), card);
    REQUIRE(all.startsWith(
        QStringLiteral("<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>")));
    REQUIRE(all.contains(QStringLiteral("<li data-dictionary=\"Dict A\"><i>(v1, vt, Dict A)</i> "
                                        "<ul><li>to eat</li><li>to live on</li></ul></li>")));
    REQUIRE(all.contains(QStringLiteral("data-dictionary=\"Dict B\"")));
    const QString first = One(QStringLiteral("{glossary-first}"), card);
    REQUIRE(first.contains(QStringLiteral("Dict A")));
    REQUIRE_FALSE(first.contains(QStringLiteral("Dict B")));
}

TEST_CASE("CardBuilder: media, text around markers and unknown markers", "[reader][cards]") {
    CardContext card = WordCard();
    REQUIRE(One(QStringLiteral("{screenshot}{audio}"), card).isEmpty());
    card.screenshot_name = QStringLiteral("a\"b.jpg");
    card.audio_name = QStringLiteral("clip.ogg");
    REQUIRE(One(QStringLiteral("<div>{screenshot}</div> {audio}"), card) ==
            QStringLiteral("<div><img src=\"a&quot;b.jpg\"></div> [sound:clip.ogg]"));
    // A typo stays visible on the card rather than silently empty.
    REQUIRE(One(QStringLiteral("{expresion} {expression}"), card) == Q(u"{expresion} 食べる"));
    REQUIRE(One(QStringLiteral("no markers }{ here"), card) ==
            QStringLiteral("no markers }{ here"));
}

TEST_CASE("CardBuilder: a card of the sentence alone", "[reader][cards]") {
    CardContext card = WordCard();
    card.word.reset();
    card.word_first = -1;
    card.word_last = -1;
    // The line stands in for the word, so that a note type made for words still gets a note.
    REQUIRE(One(QStringLiteral("{expression}"), card) ==
            Q(u"昨日、&lt;魚&gt;を食べた。<br>おいしかった"));
    REQUIRE(One(QStringLiteral("{furigana}"), card) ==
            Q(u"昨日[きのう]、&lt;魚&gt;を食べた。<br>おいしかった"));
    REQUIRE(One(QStringLiteral("{reading}{glossary}{frequencies}{inflection}"), card).isEmpty());
    REQUIRE(One(QStringLiteral("{sentence}"), card) ==
            Q(u"昨日、&lt;魚&gt;を食べた。<br>おいしかった"));
}

TEST_CASE("CardBuilder: which media a mapping asks for", "[reader][cards]") {
    REQUIRE(CardBuilder::NeedsScreenshot({QStringLiteral("{sentence}<br>{screenshot}")}));
    REQUIRE_FALSE(CardBuilder::NeedsScreenshot({QStringLiteral("{sentence}")}));
    REQUIRE(CardBuilder::NeedsAudio({QStringLiteral("x"), QStringLiteral("{audio}")}));
    REQUIRE(CardBuilder::MarkersIn({QStringLiteral("{a-b} {c}{c}")}) ==
            QSet<QString>{QStringLiteral("a-b"), QStringLiteral("c")});
}

TEST_CASE("CardBuilder: mappings are stored and suggested", "[reader][cards]") {
    const QMap<QString, QString> mapping{{QStringLiteral("Word"), QStringLiteral("{expression}")},
                                         {Q(u"文"), QStringLiteral("{sentence}")}};
    REQUIRE(CardBuilder::ParseMapping(CardBuilder::MappingToJson(mapping)) == mapping);
    REQUIRE(CardBuilder::ParseMapping(QString()).isEmpty());
    REQUIRE(CardBuilder::ParseMapping(QStringLiteral("broken")).isEmpty());

    const auto suggested = CardBuilder::SuggestMapping(
        {QStringLiteral("Expression"), QStringLiteral("Expression Furigana"),
         QStringLiteral("Expression_Reading"), QStringLiteral("Sentence"),
         QStringLiteral("SentenceAudio"), QStringLiteral("Picture"),
         QStringLiteral("MainDefinition"), QStringLiteral("FreqSort"),
         QStringLiteral("Pitch-Position"), QStringLiteral("Misc Info"), QStringLiteral("Hint")});
    REQUIRE(suggested.value(QStringLiteral("Expression")) == QStringLiteral("{expression}"));
    REQUIRE(suggested.value(QStringLiteral("Expression Furigana")) == QStringLiteral("{furigana}"));
    REQUIRE(suggested.value(QStringLiteral("Expression_Reading")) == QStringLiteral("{reading}"));
    REQUIRE(suggested.value(QStringLiteral("Sentence")) == QStringLiteral("{sentence}"));
    REQUIRE(suggested.value(QStringLiteral("SentenceAudio")) == QStringLiteral("{audio}"));
    REQUIRE(suggested.value(QStringLiteral("Picture")) == QStringLiteral("{screenshot}"));
    REQUIRE(suggested.value(QStringLiteral("MainDefinition")) == QStringLiteral("{glossary}"));
    REQUIRE(suggested.value(QStringLiteral("FreqSort")) == QStringLiteral("{frequency-rank}"));
    REQUIRE(suggested.value(QStringLiteral("Pitch-Position")) ==
            QStringLiteral("{pitch-positions}"));
    REQUIRE(suggested.value(QStringLiteral("Misc Info")) == QStringLiteral("{source}"));
    REQUIRE(ReaderApp::CardBuilder::SuggestMapping({QStringLiteral("Source URL")})
                .value(QStringLiteral("Source URL")) == QStringLiteral("{url}"));
    REQUIRE(suggested.contains(QStringLiteral("Hint")));
    REQUIRE(suggested.value(QStringLiteral("Hint")).isEmpty());
}
