// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.dict.GlossaryRenderer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Glossaries cut down for the compact popup. The entries are made up, in JMdict's structure. */
class CompactGlossaryTest {
    private fun compact(json: String) = GlossaryRenderer.compact(json).map { it.glosses to it.notes }

    @Test
    fun plainGlossesStayAsTheyAre() {
        assertEquals(
            listOf("to keep watch" to "", "to stand guard" to ""),
            compact("""["to keep watch", "to stand guard"]""")
        )
        assertEquals(listOf("first line; second line" to ""), compact("""["first line\n  second line"]"""))
        assertEquals(listOf("typed text" to ""), compact("""[{"type":"text","text":"typed text"}]"""))
        assertTrue(compact("""[{"type":"image","path":"a.png"}]""").isEmpty())
        assertTrue(compact("not json").isEmpty())
    }

    @Test
    fun markedGlossesAreRunTogetherWithoutBullets() {
        val json = """[{"type":"structured-content","content":
            {"tag":"ul","data":{"content":"glossary"},"lang":"en","style":{"listStyleType":"circle"},"content":[
                {"tag":"li","content":"good"},{"tag":"li","content":"fine"},{"tag":"li","content":"pleasant"}]}}]"""
        assertEquals(listOf("good; fine; pleasant" to ""), compact(json))
        // A list of one is an object rather than an array.
        val single = """[{"type":"structured-content","content":
            {"tag":"ul","data":{"content":"glossary"},"content":{"tag":"li","content":"to call out"}}}]"""
        assertEquals(listOf("to call out" to ""), compact(single))
    }

    @Test
    fun notesAreKeptAndCrossReferencesAndExamplesLeftOut() {
        val json = """[{"type":"structured-content","content":[
            {"tag":"ul","data":{"content":"glossary"},"content":[
                {"tag":"li","content":"OK"},{"tag":"li","content":"all right"}]},
            {"tag":"ul","data":{"content":"notes"},"content":{"tag":"li","content":"shows permission"}},
            {"tag":"ul","data":{"content":"references"},"content":{"tag":"li","content":["see: ",
                {"tag":"span","content":[{"tag":"a","href":"?query=x","content":"良い"}]},
                {"tag":"span","data":{"content":"refGlosses"},"content":" 1. good; fine"}]}},
            {"tag":"div","data":{"content":"extra-info"},"content":
                {"tag":"div","data":{"content":"example-sentence"},"content":"これはいい。"}}]}]"""
        assertEquals(listOf("OK; all right" to "shows permission"), compact(json))
    }

    @Test
    fun aDictionaryThatMarksNothingIsKeptWhole() {
        val json = """[{"type":"structured-content","content":[
            {"tag":"div","style":{"fontWeight":"bold"},"content":"food"},
            {"tag":"ul","content":[{"tag":"li","content":"item one"},
                {"tag":"li","content":["item ",{"tag":"ruby","content":["二",{"tag":"rt","content":"に"}]}]}]},
            {"tag":"table","content":{"tag":"tr","content":[{"tag":"th","content":"a"},{"tag":"td","content":"b"}]}},
            "tail",{"tag":"br"},"end"]}]"""
        assertEquals(listOf("food; item one; item 二; a b; tail; end" to ""), compact(json))
    }
}
