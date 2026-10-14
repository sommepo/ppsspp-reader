// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/glossary.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

namespace {

using Pairs = QList<std::pair<QString, QString>>;

/// Glossaries cut down for the compact popup. The entries are made up, in JMdict's structure.
Pairs Compact(const QString& json) {
    Pairs out;
    for (const Glossary::Compact& definition : Glossary::CompactDefinitions(json)) {
        out.append({definition.glosses, definition.notes});
    }
    return out;
}

} // namespace

TEST_CASE("Glossary: plain glosses stay as they are", "[reader][glossary]") {
    REQUIRE(Compact(Q(uR"(["to keep watch", "to stand guard"])")) ==
            Pairs{{QStringLiteral("to keep watch"), QString()},
                  {QStringLiteral("to stand guard"), QString()}});
    REQUIRE(Compact(Q(uR"(["first line\n  second line"])")) ==
            Pairs{{QStringLiteral("first line; second line"), QString()}});
    REQUIRE(Compact(Q(uR"([{"type":"text","text":"typed text"}])")) ==
            Pairs{{QStringLiteral("typed text"), QString()}});
    REQUIRE(Compact(Q(uR"([{"type":"image","path":"a.png"}])")).isEmpty());
    REQUIRE(Compact(QStringLiteral("not json")).isEmpty());
}

TEST_CASE("Glossary: marked glosses are run together without bullets", "[reader][glossary]") {
    const QString json = Q(uR"([{"type":"structured-content","content":
            {"tag":"ul","data":{"content":"glossary"},"lang":"en","style":{"listStyleType":"circle"},"content":[
                {"tag":"li","content":"good"},{"tag":"li","content":"fine"},{"tag":"li","content":"pleasant"}]}}])");
    REQUIRE(Compact(json) == Pairs{{QStringLiteral("good; fine; pleasant"), QString()}});
    // A list of one is an object rather than an array.
    const QString single = Q(uR"([{"type":"structured-content","content":
            {"tag":"ul","data":{"content":"glossary"},"content":{"tag":"li","content":"to call out"}}}])");
    REQUIRE(Compact(single) == Pairs{{QStringLiteral("to call out"), QString()}});
}

TEST_CASE("Glossary: notes are kept; cross-references and examples are left out",
          "[reader][glossary]") {
    const QString json = Q(uR"([{"type":"structured-content","content":[
            {"tag":"ul","data":{"content":"glossary"},"content":[
                {"tag":"li","content":"OK"},{"tag":"li","content":"all right"}]},
            {"tag":"ul","data":{"content":"notes"},"content":{"tag":"li","content":"shows permission"}},
            {"tag":"ul","data":{"content":"references"},"content":{"tag":"li","content":["see: ",
                {"tag":"span","content":[{"tag":"a","href":"?query=x","content":"良い"}]},
                {"tag":"span","data":{"content":"refGlosses"},"content":" 1. good; fine"}]}},
            {"tag":"div","data":{"content":"extra-info"},"content":
                {"tag":"div","data":{"content":"example-sentence"},"content":"これはいい。"}}]}])");
    REQUIRE(Compact(json) ==
            Pairs{{QStringLiteral("OK; all right"), QStringLiteral("shows permission")}});
}

TEST_CASE("Glossary: a dictionary that marks nothing is kept whole", "[reader][glossary]") {
    const QString json = Q(uR"([{"type":"structured-content","content":[
            {"tag":"div","style":{"fontWeight":"bold"},"content":"food"},
            {"tag":"ul","content":[{"tag":"li","content":"item one"},
                {"tag":"li","content":["item ",{"tag":"ruby","content":["二",{"tag":"rt","content":"に"}]}]}]},
            {"tag":"table","content":{"tag":"tr","content":[{"tag":"th","content":"a"},{"tag":"td","content":"b"}]}},
            "tail",{"tag":"br"},"end"]}])");
    REQUIRE(Compact(json) ==
            Pairs{{Q(u"food; item one; item 二; a b; tail; end"), QString()}});
}

TEST_CASE("Glossary: links and table cells keep only what is safe", "[reader][glossary]") {
    const QString json = Q(uR"([{"type":"structured-content","content":[
            {"tag":"a","href":"https://example.org/x?a=1&b=2","content":"out"},
            {"tag":"a","href":"?query=inside","content":"in"},
            {"tag":"table","content":{"tag":"tr","content":
                {"tag":"td","colSpan":2,"style":{"textAlign":"center","bad;key":"x","color":"red;}"},"content":"cell"}}}]}])");
    const QString html = Glossary::ToHtml(json, Glossary::Mode::Full);
    REQUIRE(html.contains(
        QStringLiteral("<a href=\"https://example.org/x?a=1&amp;b=2\">out</a>")));
    REQUIRE(html.contains(QStringLiteral("<a>in</a>")));
    REQUIRE(html.contains(QStringLiteral("colspan=\"2\"")));
    REQUIRE(html.contains(QStringLiteral("text-align: center;")));
    REQUIRE_FALSE(html.contains(QStringLiteral("red")));
    REQUIRE_FALSE(html.contains(QStringLiteral("bad")));
}
