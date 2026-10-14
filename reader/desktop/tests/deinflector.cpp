// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/deinflector.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

/// True when `source` can be reduced to `term` as a word of `word_class`.
bool Reaches(const Deinflector& deinflector, const QString& source, const QString& term,
             int word_class) {
    const QList<Deinflection> results = deinflector.Deinflect(source);
    return std::any_of(results.begin(), results.end(), [&](const Deinflection& d) {
        return d.term == term && (d.conditions & word_class) != 0;
    });
}

void RequireAll(const Deinflector& deinflector, const QString& term, int word_class,
                std::initializer_list<const char16_t*> forms) {
    for (const char16_t* form : forms) {
        INFO(Q(form).toStdString() << " should reduce to " << term.toStdString());
        REQUIRE(Reaches(deinflector, Q(form), term, word_class));
    }
}

const Deinflection* First(const QList<Deinflection>& results, const QString& term, int conditions) {
    for (const Deinflection& d : results) {
        if (d.term == term && d.conditions == conditions) {
            return &d;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Deinflector: the source itself comes first", "[reader][deinflector]") {
    const Deinflector deinflector;
    const QList<Deinflection> results = deinflector.Deinflect(Q(u"食べる"));
    REQUIRE(results.at(0).term == Q(u"食べる"));
    REQUIRE(results.at(0).conditions == 0);
    REQUIRE(results.at(0).reasons.isEmpty());
}

TEST_CASE("Deinflector: ichidan verbs", "[reader][deinflector]") {
    const Deinflector deinflector;
    RequireAll(deinflector, Q(u"食べる"), WordClass::V1,
               {u"食べた",       u"食べて",         u"食べない",     u"食べます",
                u"食べません",   u"食べました",     u"食べませんでした", u"食べましょう",
                u"食べたい",     u"食べよう",       u"食べろ",       u"食べれば",
                u"食べたら",     u"食べたり",       u"食べられる",   u"食べさせる",
                u"食べさせられる", u"食べている",     u"食べてる",     u"食べていた",
                u"食べなかった", u"食べなくて",     u"食べたくない", u"食べたかった",
                u"食べちゃう",   u"食べちゃった",   u"食べとく",     u"食べず",
                u"食べぬ",       u"食べなさい",     u"食べそう",     u"食べすぎる",
                u"食べすぎた",   u"食べてしまう",   u"食べてしまった", u"食べ",
                u"食べなきゃ",   u"食べるな",       u"食べれる",     u"食べられない",
                u"食べさせられた", u"食べてた",       u"食べなければ"});
}

TEST_CASE("Deinflector: godan verbs of every ending", "[reader][deinflector]") {
    const Deinflector deinflector;
    const int v5 = WordClass::V5;
    RequireAll(deinflector, Q(u"買う"), v5,
               {u"買った", u"買って", u"買わない", u"買います", u"買える", u"買われる", u"買わせる",
                u"買おう", u"買えば", u"買い", u"買っちゃう", u"買っている", u"買わなかった"});
    RequireAll(deinflector, Q(u"書く"), v5,
               {u"書いた", u"書いて", u"書かない", u"書きます", u"書ける", u"書かれる", u"書こう",
                u"書けば", u"書き", u"書いちゃった", u"書いとく"});
    RequireAll(deinflector, Q(u"泳ぐ"), v5,
               {u"泳いだ", u"泳いで", u"泳がない", u"泳ぎます", u"泳げる", u"泳ごう", u"泳いじゃう",
                u"泳いどく", u"泳いだら"});
    RequireAll(deinflector, Q(u"話す"), v5,
               {u"話した", u"話して", u"話さない", u"話します", u"話せる", u"話そう", u"話せば",
                u"話し", u"話しちゃう"});
    RequireAll(deinflector, Q(u"待つ"), v5,
               {u"待った", u"待って", u"待たない", u"待ちます", u"待てる", u"待とう", u"待てば"});
    RequireAll(deinflector, Q(u"死ぬ"), v5,
               {u"死んだ", u"死んで", u"死なない", u"死にます", u"死ねる", u"死のう", u"死んじゃう"});
    RequireAll(deinflector, Q(u"遊ぶ"), v5,
               {u"遊んだ", u"遊んで", u"遊ばない", u"遊びます", u"遊べる", u"遊ぼう", u"遊んだり"});
    RequireAll(deinflector, Q(u"読む"), v5,
               {u"読んだ", u"読んで", u"読まない", u"読みます", u"読める", u"読もう", u"読まれる",
                u"読ませる", u"読まされる", u"読んでいる", u"読んでた", u"読まず"});
    RequireAll(deinflector, Q(u"取る"), v5,
               {u"取った", u"取って", u"取らない", u"取ります", u"取れる", u"取ろう", u"取れば",
                u"取り", u"取られた"});
    // 行く is the godan verb in く that takes って.
    RequireAll(deinflector, Q(u"行く"), v5, {u"行った", u"行って", u"行っちゃった"});
}

TEST_CASE("Deinflector: irregular verbs", "[reader][deinflector]") {
    const Deinflector deinflector;
    for (const char16_t* form : {u"した", u"して", u"しない", u"します", u"しよう", u"すれば",
                                 u"される", u"させる", u"している", u"しなかった", u"せず",
                                 u"しちゃった", u"したい"}) {
        INFO(Q(form).toStdString());
        REQUIRE(Reaches(deinflector, Q(u"勉強") + Q(form), Q(u"勉強する"), WordClass::VS));
    }
    RequireAll(deinflector, Q(u"くる"), WordClass::VK,
               {u"きた", u"きて", u"こない", u"きます", u"こよう", u"くれば", u"こられる",
                u"こさせる", u"きている", u"こなかった"});
    RequireAll(deinflector, Q(u"来る"), WordClass::VK,
               {u"来た", u"来て", u"来ない", u"来ます", u"来よう", u"来れば", u"来られる"});
}

TEST_CASE("Deinflector: adjectives", "[reader][deinflector]") {
    const Deinflector deinflector;
    RequireAll(deinflector, Q(u"高い"), WordClass::ADJ_I,
               {u"高かった", u"高くない", u"高くて", u"高く", u"高ければ", u"高さ", u"高そう",
                u"高すぎる", u"高くなかった", u"高かったら", u"高かろう"});
    REQUIRE(Reaches(deinflector, Q(u"悲しげ"), Q(u"悲しい"), WordClass::ADJ_I));
    REQUIRE(Reaches(deinflector, Q(u"知らねえ"), Q(u"知る"), WordClass::V5));
}

TEST_CASE("Deinflector: reasons are listed outermost first", "[reader][deinflector]") {
    const Deinflector deinflector;
    const QList<Deinflection> long_form = deinflector.Deinflect(Q(u"読まれなかった"));
    const Deinflection* result = First(long_form, Q(u"読む"), WordClass::V5);
    REQUIRE(result != nullptr);
    REQUIRE(result->reasons ==
            QStringList{QStringLiteral("past"), QStringLiteral("negative"), QStringLiteral("passive")});
    // When two chains reach the same word, the shorter one is reported.
    const QList<Deinflection> causative = deinflector.Deinflect(Q(u"食べさせられた"));
    const Deinflection* shortest = First(causative, Q(u"食べる"), WordClass::V1);
    REQUIRE(shortest != nullptr);
    REQUIRE(shortest->reasons ==
            QStringList{QStringLiteral("past"), QStringLiteral("causative passive")});
}

TEST_CASE("Deinflector: word classes keep unrelated forms apart", "[reader][deinflector]") {
    const Deinflector deinflector;
    // 食べた is not a godan form of anything: no v5 candidate may be called 食べる.
    REQUIRE_FALSE(Reaches(deinflector, Q(u"食べた"), Q(u"食べる"), WordClass::V5));
    // The te-form marker is internal and never a class a dictionary entry could have.
    REQUIRE(First(deinflector.Deinflect(Q(u"読んでいる")), Q(u"読む"), WordClass::TE) == nullptr);
}

TEST_CASE("Deinflector: stays bounded on long input", "[reader][deinflector]") {
    const Deinflector deinflector;
    REQUIRE(deinflector.Deinflect(Q(u"させられなかったらしくてならなかった")).size() <= 400);
}

TEST_CASE("Deinflector: dictionary rules are parsed", "[reader][deinflector]") {
    REQUIRE(WordClass::Parse(QStringLiteral("v1")) == WordClass::V1);
    REQUIRE(WordClass::Parse(QStringLiteral("v5")) == WordClass::V5);
    REQUIRE(WordClass::Parse(QStringLiteral("v1 vs")) == (WordClass::V1 | WordClass::VS));
    REQUIRE(WordClass::Parse(QStringLiteral("adj-i")) == WordClass::ADJ_I);
    REQUIRE(WordClass::Parse(QString()) == 0);
    REQUIRE(WordClass::Parse(QStringLiteral("n")) == 0);
}
