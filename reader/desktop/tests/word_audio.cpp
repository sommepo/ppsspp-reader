// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

// The word's own sound on a card: where it is asked for, what counts as a recording, and the
// marker and fields it goes into. Nothing here reaches the network: the fetcher is a stand-in.

#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/card_builder.h"
#include "reader/desktop/word_audio.h"

using namespace ReaderApp;

TEST_CASE("WordAudio: a word is asked for by its writing and its reading", "[reader][cards]") {
    REQUIRE(WordAudio::Address(QStringLiteral("作り話"), QStringLiteral("つくりばなし")) ==
            QStringLiteral("https://assets.languagepod101.com/dictionary/japanese/audiomp3.php"
                           "?kanji=%E4%BD%9C%E3%82%8A%E8%A9%B1"
                           "&kana=%E3%81%A4%E3%81%8F%E3%82%8A%E3%81%B0%E3%81%AA%E3%81%97"));
    // A word in kana alone, and one whose reading is not known, by what there is.
    REQUIRE(WordAudio::Address(QStringLiteral("はなし"), QStringLiteral("はなし"))
                .endsWith(QStringLiteral("audiomp3.php?kana=%E3%81%AF%E3%81%AA%E3%81%97")));
    REQUIRE(WordAudio::Address(QStringLiteral("ハナシ"), QString())
                .endsWith(QStringLiteral("audiomp3.php?kana=%E3%83%8F%E3%83%8A%E3%82%B7")));
}

TEST_CASE("WordAudio: what is fetched is a recording, or nothing", "[reader][cards]") {
    QByteArray recording(3000, 'x');
    for (int i = 0; i < recording.size(); ++i) {
        recording[i] = static_cast<char>(i % 251);
    }
    REQUIRE(WordAudio::IsRecording(recording));
    REQUIRE_FALSE(WordAudio::IsRecording({}));
    REQUIRE_FALSE(WordAudio::IsRecording(QByteArray(40, 'x'))); // too short to be sound
    REQUIRE(QByteArray{WordAudio::Missing}.size() == 64);

    QString asked;
    const QByteArray got = WordAudio::Fetch(QStringLiteral("作り話"), QStringLiteral("つくりばなし"),
                                            [&](const QString& address) {
                                                asked = address;
                                                return recording;
                                            });
    REQUIRE(got == recording);
    REQUIRE(asked == WordAudio::Address(QStringLiteral("作り話"), QStringLiteral("つくりばなし")));
    // Nothing to be had, or the server not reached: no recording, and no failure.
    REQUIRE(WordAudio::Fetch(QStringLiteral("作り話"), QStringLiteral("つくりばなし"),
                             [](const QString&) { return QByteArray{}; })
                .isEmpty());
    REQUIRE(WordAudio::Fetch(QStringLiteral("作り話"), QStringLiteral("つくりばなし"),
                             [](const QString&) -> QByteArray { throw 1; })
                .isEmpty());
    // No word, nothing asked.
    bool called = false;
    REQUIRE(WordAudio::Fetch(QString(), QString(), [&](const QString&) {
                called = true;
                return recording;
            }).isEmpty());
    REQUIRE_FALSE(called);
}

TEST_CASE("WordAudio: the same word is kept under the same name", "[reader][cards]") {
    const QString name = WordAudio::FileName(QStringLiteral("作り話"), QStringLiteral("つくりばなし"));
    REQUIRE(name.startsWith(QStringLiteral("reader_word_")));
    REQUIRE(name.endsWith(QStringLiteral(".mp3")));
    REQUIRE(name.size() == 12 + 16 + 4);
    REQUIRE(name == WordAudio::FileName(QStringLiteral("作り話"), QStringLiteral("つくりばなし")));
    REQUIRE(name != WordAudio::FileName(QStringLiteral("作り話"), QStringLiteral("つくりはなし")));
}

TEST_CASE("WordAudio: a field for the word's sound gets the marker", "[reader][cards]") {
    const auto guessed = CardBuilder::SuggestMapping(
        {QStringLiteral("Word"), QStringLiteral("WordAudio"), QStringLiteral("SentenceAudio"),
         QStringLiteral("Audio"), QStringLiteral("Notes")});
    REQUIRE(guessed.value(QStringLiteral("WordAudio")) == QStringLiteral("{word-audio}"));
    REQUIRE(guessed.value(QStringLiteral("Audio")) == QStringLiteral("{word-audio}"));
    REQUIRE(guessed.value(QStringLiteral("SentenceAudio")) == QStringLiteral("{audio}"));
    REQUIRE(guessed.value(QStringLiteral("Notes")).isEmpty());
    REQUIRE(CardBuilder::NeedsWordAudio(guessed.values()));
    REQUIRE_FALSE(CardBuilder::NeedsWordAudio({QStringLiteral("{audio}"), QStringLiteral("{expression}")}));

    // An older mapping gets the marker where it left the word's sound empty, and only there.
    const QMap<QString, QString> older{{QStringLiteral("Word"), QStringLiteral("{expression}")},
                                       {QStringLiteral("WordAudio"), QString()},
                                       {QStringLiteral("SentenceAudio"), QStringLiteral("{audio}")},
                                       {QStringLiteral("Hint"), QString()}};
    const auto offered = CardBuilder::OfferWordAudio(older);
    REQUIRE(offered.value(QStringLiteral("WordAudio")) == QStringLiteral("{word-audio}"));
    REQUIRE(offered.value(QStringLiteral("Hint")).isEmpty());
    REQUIRE(offered.value(QStringLiteral("SentenceAudio")) == QStringLiteral("{audio}"));
    REQUIRE(CardBuilder::OfferWordAudio({{QStringLiteral("WordAudio"), QStringLiteral("mine")}})
                .value(QStringLiteral("WordAudio")) == QStringLiteral("mine"));

    CardContext card;
    card.word_audio_name = QStringLiteral("reader_word_0123.mp3");
    REQUIRE(CardBuilder::Build({{QStringLiteral("A"), QStringLiteral("{word-audio}")}}, card)
                .value(QStringLiteral("A")) == QStringLiteral("[sound:reader_word_0123.mp3]"));
}
