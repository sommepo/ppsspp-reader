// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QTemporaryDir>
#include <quazip.h>
#include <quazipfile.h>
#include "reader/desktop/dictionary.h"
#include "reader/desktop/glossary.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

using Files = QList<std::pair<QString, QString>>;

/// Dictionary import and lookup against small archives written for the test (no real data).
struct Fixture {
    QTemporaryDir folder;
    DictionaryDatabase db{QStringLiteral(":memory:")};
    DictionaryImporter importer{db};
    DictionaryLookup lookup{db};

    QString Archive(const QString& name, const Files& files) {
        const QString path = folder.filePath(name);
        QFile::remove(path);
        QuaZip zip(path);
        REQUIRE(zip.open(QuaZip::mdCreate));
        for (const auto& [entry, content] : files) {
            QuaZipFile file(&zip);
            REQUIRE(file.open(QIODevice::WriteOnly, QuaZipNewInfo(entry)));
            file.write(content.toUtf8());
            file.close();
        }
        zip.close();
        return path;
    }

    QString Structured() const {
        return Q(uR"({"type":"structured-content","content":[
        {"tag":"div","style":{"fontWeight":"bold"},"data":{"content":"gloss"},"content":"food"},
        {"tag":"ul","content":[{"tag":"li","content":"item one"},
          {"tag":"li","content":["item ",{"tag":"ruby","content":["二",{"tag":"rt","content":"に"}]}]}]},
        {"tag":"img","path":"x.png"}]})");
    }

    QString Terms() const {
        return Q(uR"json([
        ["食べる","たべる","v1","v1",100,["to eat","to live on"],1,""],
        ["読む","よむ","v5","v5",90,["to read"],2,""],
        ["高い","たかい","adj-i","adj-i",80,["high","expensive"],3,""],
        ["これ","","pn","",70,["this"],4,""],
        ["食べ物","たべもの","n","",60,[)json") +
               Structured() +
               Q(uR"json(],5,""],
        ["名","な","n","",50,["name"],6,""],
        ["名乗る","なのる","v5","v5",50,["to give one's name"],7,""],
        ["マリア","","n","",1,["Maria <the name>"],8,""],
        ["飼う","かう","v5","v5",40,["to keep (a pet)"],9,""],
        ["買う","かう","v5","v5",95,["to buy"],10,""]
    ])json");
    }

    QString Main() {
        return Archive(
            QStringLiteral("main.zip"),
            {
                // Deliberately not first in the archive: the importer must not rely on entry
                // order.
                {QStringLiteral("term_bank_1.json"), Terms()},
                {QStringLiteral("index.json"),
                 Q(uR"({"title":"Test Dict","revision":"r1","format":3,"author":"tests"})")},
                {QStringLiteral("tag_bank_1.json"), Q(uR"([["n","partOfSpeech",0,"noun",0]])")},
            });
    }

    QString FrequencyArchive() {
        return Archive(
            QStringLiteral("freq.zip"),
            {{QStringLiteral("index.json"),
              Q(uR"({"title":"Freq","revision":"1","format":3,"frequencyMode":"rank-based"})")},
             {QStringLiteral("term_meta_bank_1.json"), Q(uR"([
                ["食べる","freq",1500],
                ["読む","freq",{"reading":"よむ","frequency":{"value":800,"displayValue":"800㋕"}}],
                ["読む","freq",{"reading":"どく","frequency":5}],
                ["読む","pitch",{"reading":"よむ","pitches":[{"position":1},{"position":0}]}],
                ["買う","freq","rank 200"]
            ])")}});
    }

    /// A monolingual-style dictionary: no word classes at all.
    QString Mono() {
        return Archive(QStringLiteral("mono.zip"),
                       {{QStringLiteral("index.json"),
                         Q(uR"({"title":"Mono","revision":"1","format":3})")},
                        {QStringLiteral("term_bank_1.json"),
                         Q(uR"([["食べる","たべる","","",0,["食物を口に入れる。"],0,""],
                ["た","","","",0,["助動詞。"],0,""]])")}});
    }
};

QStringList Expressions(const QList<LookupResult>& results) {
    QStringList out;
    for (const LookupResult& result : results) {
        out.append(result.expression);
    }
    return out;
}

QStringList DictionariesOf(const LookupResult& result) {
    QStringList out;
    for (const TermEntry& entry : result.entries) {
        out.append(entry.dictionary);
    }
    return out;
}

QStringList Titles(DictionaryDatabase& db) {
    QStringList out;
    for (const DictionaryInfo& info : db.Dictionaries()) {
        out.append(info.title);
    }
    return out;
}

} // namespace

TEST_CASE("Dictionary: terms are imported and progress reported", "[reader][dictionary]") {
    Fixture f;
    QList<std::pair<int, int>> steps;
    const auto result = f.importer.Import(
        f.Main(), [&steps](int done, int total, const QString&) { steps.append({done, total}); });
    REQUIRE(result.ok);
    REQUIRE(result.title == QStringLiteral("Test Dict"));
    REQUIRE(result.terms == 10);
    REQUIRE(result.metas == 0);
    REQUIRE(steps == QList<std::pair<int, int>>{{0, 1}, {1, 1}});
    const QList<DictionaryInfo> all = f.db.Dictionaries();
    REQUIRE(all.size() == 1);
    REQUIRE(all.first().title == QStringLiteral("Test Dict"));
    REQUIRE(all.first().revision == QStringLiteral("r1"));
    REQUIRE(all.first().term_count == 10);
    REQUIRE(all.first().enabled);
}

TEST_CASE("Dictionary: duplicates and broken archives are refused", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const auto again = f.importer.Import(f.Main());
    REQUIRE_FALSE(again.ok);
    REQUIRE(again.error.contains(QStringLiteral("already installed")));
    REQUIRE_FALSE(f.importer
                      .Import(f.Archive(QStringLiteral("empty.zip"),
                                        {{QStringLiteral("readme.txt"), QStringLiteral("hello")}}))
                      .ok);
    REQUIRE_FALSE(f.importer
                      .Import(f.Archive(QStringLiteral("noterms.zip"),
                                        {{QStringLiteral("index.json"),
                                          Q(uR"({"title":"X","format":3})")}}))
                      .ok);
    // A bank that breaks half way leaves nothing behind.
    REQUIRE_FALSE(
        f.importer
            .Import(f.Archive(QStringLiteral("broken.zip"),
                              {{QStringLiteral("index.json"), Q(uR"({"title":"Broken","format":3})")},
                               {QStringLiteral("term_bank_1.json"),
                                Q(uR"([["良い","よい","","adj-i",1,["good"],1,""],["oops)")}}))
            .ok);
    REQUIRE_FALSE(f.importer.Import(f.folder.filePath(QStringLiteral("missing.zip"))).ok);
    REQUIRE(Titles(f.db) == QStringList{QStringLiteral("Test Dict")});
    REQUIRE(f.lookup.Lookup(Q(u"良い")).isEmpty());
}

TEST_CASE("Dictionary: a cancelled import leaves the database unchanged", "[reader][dictionary]") {
    Fixture f;
    const auto result = f.importer.Import(f.Main(), {}, [] { return false; });
    REQUIRE_FALSE(result.ok);
    REQUIRE(f.db.Dictionaries().isEmpty());
}

TEST_CASE("Dictionary: inflected words are found, with why", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const auto past = f.lookup.Lookup(Q(u"食べた。それから"));
    REQUIRE_FALSE(past.isEmpty());
    REQUIRE(past.at(0).expression == Q(u"食べる"));
    REQUIRE(past.at(0).reading == Q(u"たべる"));
    REQUIRE(past.at(0).matched_text == Q(u"食べた"));
    REQUIRE(past.at(0).reasons == QStringList{QStringLiteral("past")});
    const auto progressive = f.lookup.Lookup(Q(u"読んでいる"));
    REQUIRE(progressive.at(0).expression == Q(u"読む"));
    REQUIRE(progressive.at(0).matched_text == Q(u"読んでいる"));
    REQUIRE(progressive.at(0).reasons ==
            QStringList{QStringLiteral("progressive or perfect"), QStringLiteral("-te")});
    const auto imperative = f.lookup.Lookup(Q(u"名乗れ"));
    REQUIRE(Expressions(imperative) == QStringList{Q(u"名乗る"), Q(u"名")});
    REQUIRE(imperative.at(0).matched_text == Q(u"名乗れ"));
    REQUIRE(imperative.at(1).matched_text == Q(u"名"));
}

TEST_CASE("Dictionary: the longest match comes first", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const auto results = f.lookup.Lookup(Q(u"食べ物を買う"));
    REQUIRE(results.at(0).expression == Q(u"食べ物"));
    // The shorter reading of the same text is still offered after it.
    REQUIRE(std::any_of(results.begin(), results.end(), [](const LookupResult& r) {
        return r.expression == Q(u"食べる") && r.matched_text == Q(u"食べ");
    }));
}

TEST_CASE("Dictionary: kana matches by reading, katakana as written", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const auto by_reading = f.lookup.Lookup(Q(u"たかくない"));
    REQUIRE(by_reading.at(0).expression == Q(u"高い"));
    REQUIRE(by_reading.at(0).matched_text == Q(u"たかくない"));
    // Two words share the reading かう: both are offered, the higher score first.
    REQUIRE(Expressions(f.lookup.Lookup(Q(u"かった"))) == QStringList{Q(u"買う"), Q(u"飼う")});
    REQUIRE(f.lookup.Lookup(Q(u"マリアだ")).at(0).expression == Q(u"マリア"));
    REQUIRE(f.lookup.Lookup(Q(u"マリアだ")).at(0).DisplayReading().isEmpty());
    REQUIRE(f.lookup.Lookup(Q(u"これは")).at(0).expression == Q(u"これ"));
}

TEST_CASE("Dictionary: the scan stops at punctuation", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    REQUIRE(f.lookup.Lookup(QString()).isEmpty());
    REQUIRE(f.lookup.Lookup(Q(u"、食べる")).isEmpty());
    REQUIRE(f.lookup.Lookup(Q(u"…")).isEmpty());
    // 食べる。読む: the scan must not run across the full stop.
    REQUIRE(f.lookup.Lookup(Q(u"食べる。読む")).at(0).matched_text == Q(u"食べる"));
}

TEST_CASE("Dictionary: word classes rule out false deinflections", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    // 名た would be a "past" of an ichidan 名る; 名 is a noun, so only the plain 名 matches.
    const auto results = f.lookup.Lookup(Q(u"名た"));
    REQUIRE(Expressions(results) == QStringList{Q(u"名")});
    REQUIRE(results.at(0).matched_text == Q(u"名"));
    REQUIRE(results.at(0).reasons.isEmpty());
}

TEST_CASE("Dictionary: one without word classes joins confirmed headwords",
          "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    REQUIRE(f.importer.Import(f.Mono()).ok);
    const auto results = f.lookup.Lookup(Q(u"食べた"));
    REQUIRE(results.at(0).expression == Q(u"食べる"));
    REQUIRE(DictionariesOf(results.at(0)) ==
            QStringList{QStringLiteral("Test Dict"), QStringLiteral("Mono")});
    // On its own, the class-less dictionary cannot confirm an inflection...
    for (const DictionaryInfo& info : f.db.Dictionaries()) {
        if (info.title == QStringLiteral("Test Dict")) {
            f.db.SetEnabled(info.id, false);
        }
    }
    const auto alone = f.lookup.Lookup(Q(u"食べた"));
    REQUIRE(std::none_of(alone.begin(), alone.end(),
                         [](const LookupResult& r) { return r.expression == Q(u"食べる"); }));
    // ...but still matches what is written.
    REQUIRE(f.lookup.Lookup(Q(u"食べる")).at(0).expression == Q(u"食べる"));
}

TEST_CASE("Dictionary: frequencies and pitch are those of the right reading",
          "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const auto imported = f.importer.Import(f.FrequencyArchive());
    REQUIRE(imported.ok);
    REQUIRE(imported.metas == 5);
    const LookupResult yomu = f.lookup.Lookup(Q(u"読む")).at(0);
    REQUIRE(yomu.frequencies.size() == 1);
    REQUIRE(yomu.frequencies.at(0).dictionary == QStringLiteral("Freq"));
    REQUIRE(yomu.frequencies.at(0).display == Q(u"800㋕"));
    REQUIRE(yomu.frequencies.at(0).value == 800.0);
    REQUIRE(yomu.frequencies.at(0).rank_based);
    REQUIRE(yomu.pitches.size() == 1);
    REQUIRE(yomu.pitches.at(0).positions == QList<int>{1, 0});
    const LookupResult taberu = f.lookup.Lookup(Q(u"食べる")).at(0);
    REQUIRE(taberu.frequencies.size() == 1);
    REQUIRE(taberu.frequencies.at(0).display == QStringLiteral("1500"));
    REQUIRE(taberu.frequencies.at(0).value == 1500.0);
    REQUIRE(taberu.pitches.isEmpty());
    const LookupResult kau = f.lookup.Lookup(Q(u"買う")).at(0);
    REQUIRE(kau.frequencies.size() == 1);
    REQUIRE(kau.frequencies.at(0).display == QStringLiteral("rank 200"));
    REQUIRE_FALSE(kau.frequencies.at(0).value.has_value());
    REQUIRE(DictionaryLookup::HarmonicRank(taberu.frequencies) == 1500);
    REQUIRE_FALSE(DictionaryLookup::HarmonicRank(kau.frequencies).has_value());
    REQUIRE(DictionaryLookup::HarmonicRank(
                {Frequency{QStringLiteral("a"), QStringLiteral("800"), 800.0, true},
                 Frequency{QStringLiteral("b"), QStringLiteral("1500"), 1500.0, true}}) == 1043);
}

TEST_CASE("Dictionary: order, enabling and deletion are respected", "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    REQUIRE(f.importer.Import(f.Mono()).ok);
    const QList<DictionaryInfo> both = f.db.Dictionaries();
    const DictionaryInfo main = both.at(0);
    const DictionaryInfo mono = both.at(1);
    f.db.SetOrder({mono.id, main.id});
    REQUIRE(Titles(f.db) == QStringList{QStringLiteral("Mono"), QStringLiteral("Test Dict")});
    REQUIRE(DictionariesOf(f.lookup.Lookup(Q(u"食べる")).at(0)) ==
            QStringList{QStringLiteral("Mono"), QStringLiteral("Test Dict")});
    f.db.SetEnabled(mono.id, false);
    REQUIRE(DictionariesOf(f.lookup.Lookup(Q(u"食べる")).at(0)) ==
            QStringList{QStringLiteral("Test Dict")});
    REQUIRE(f.db.HasEnabledDictionaries());
    f.db.Delete(main.id);
    REQUIRE(f.lookup.Lookup(Q(u"食べる")).isEmpty());
    REQUIRE_FALSE(f.db.HasEnabledDictionaries());
    REQUIRE(Titles(f.db) == QStringList{QStringLiteral("Mono")});
}

TEST_CASE("Dictionary: glossaries are rendered for cards and for the popup",
          "[reader][dictionary]") {
    Fixture f;
    REQUIRE(f.importer.Import(f.Main()).ok);
    const QString plain = f.lookup.Lookup(Q(u"食べる")).at(0).entries.at(0).glossary_json;
    REQUIRE(Glossary::ToPlainText(plain) == QStringLiteral("to eat; to live on"));
    REQUIRE(Glossary::ToHtml(plain, Glossary::Mode::Full) ==
            QStringLiteral("<ul><li>to eat</li><li>to live on</li></ul>"));
    const QString structured = f.lookup.Lookup(Q(u"食べ物")).at(0).entries.at(0).glossary_json;
    const QString full = Glossary::ToHtml(structured, Glossary::Mode::Full);
    REQUIRE(full.contains(QStringLiteral("data-sc-content=\"gloss\"")));
    REQUIRE(full.contains(QStringLiteral("font-weight: bold")));
    REQUIRE(full.contains(Q(u"<ruby>二<rt>に</rt></ruby>")));
    REQUIRE_FALSE(full.contains(QStringLiteral("img")));
    const QString simple = Glossary::ToHtml(structured, Glossary::Mode::Simple);
    REQUIRE(simple.contains(QStringLiteral("<b>food</b>")));
    REQUIRE(simple.contains(Q(u"• item one")));
    REQUIRE_FALSE(simple.contains(Q(u"に")));
    // What is escaped in a plain gloss stays escaped as HTML and comes back as text.
    const QString maria = f.lookup.Lookup(Q(u"マリア")).at(0).entries.at(0).glossary_json;
    REQUIRE(Glossary::ToHtml(maria, Glossary::Mode::Full) ==
            QStringLiteral("Maria &lt;the name&gt;"));
    REQUIRE(Glossary::ToPlainText(maria) == QStringLiteral("Maria <the name>"));
}

TEST_CASE("Dictionary: glossaries survive packing", "[reader][dictionary]") {
    const QString small = QStringLiteral("[\"short\"]");
    REQUIRE(DictionaryDatabase::Unpack(DictionaryDatabase::Pack(small)) == small);
    QString large = QStringLiteral("[");
    for (int i = 0; i < 400; ++i) {
        large += QStringLiteral("\"definition number %1 with some text\",").arg(i);
    }
    large += Q(u"\"終わり\"]");
    const QByteArray packed = DictionaryDatabase::Pack(large);
    REQUIRE(packed.size() < large.toUtf8().size() / 2);
    REQUIRE(packed.at(0) == 1);
    REQUIRE(DictionaryDatabase::Unpack(packed) == large);
    REQUIRE(DictionaryDatabase::Unpack({}) == QStringLiteral("[]"));
}
