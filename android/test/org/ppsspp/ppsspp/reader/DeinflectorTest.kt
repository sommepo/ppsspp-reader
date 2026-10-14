// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.dict.Deinflector
import org.ppsspp.ppsspp.reader.dict.WordClass
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DeinflectorTest {
    private val deinflector = Deinflector()

    /** True when [source] can be reduced to [term] as a word of [wordClass]. */
    private fun reaches(source: String, term: String, wordClass: Int): Boolean =
        deinflector.deinflect(source).any { it.term == term && (it.conditions and wordClass) != 0 }

    private fun assertReaches(source: String, term: String, wordClass: Int) {
        assertTrue("$source should reduce to $term", reaches(source, term, wordClass))
    }

    @Test
    fun keepsTheSourceItselfFirst() {
        val results = deinflector.deinflect("食べる")
        assertEquals("食べる", results[0].term)
        assertEquals(0, results[0].conditions)
        assertTrue(results[0].reasons.isEmpty())
    }

    @Test
    fun ichidanVerbs() {
        val v1 = WordClass.V1
        for (form in listOf(
            "食べた", "食べて", "食べない", "食べます", "食べません", "食べました", "食べませんでした",
            "食べましょう", "食べたい", "食べよう", "食べろ", "食べれば", "食べたら", "食べたり",
            "食べられる", "食べさせる", "食べさせられる", "食べている", "食べてる", "食べていた",
            "食べなかった", "食べなくて", "食べたくない", "食べたかった", "食べちゃう", "食べちゃった",
            "食べとく", "食べず", "食べぬ", "食べなさい", "食べそう", "食べすぎる", "食べすぎた",
            "食べてしまう", "食べてしまった", "食べ", "食べなきゃ", "食べるな", "食べれる",
            "食べられない", "食べさせられた", "食べてた", "食べなければ"
        )) {
            assertReaches(form, "食べる", v1)
        }
    }

    @Test
    fun godanVerbsOfEveryEnding() {
        val v5 = WordClass.V5
        val cases = mapOf(
            "買う" to listOf("買った", "買って", "買わない", "買います", "買える", "買われる", "買わせる", "買おう", "買えば", "買い", "買っちゃう", "買っている", "買わなかった"),
            "書く" to listOf("書いた", "書いて", "書かない", "書きます", "書ける", "書かれる", "書こう", "書けば", "書き", "書いちゃった", "書いとく"),
            "泳ぐ" to listOf("泳いだ", "泳いで", "泳がない", "泳ぎます", "泳げる", "泳ごう", "泳いじゃう", "泳いどく", "泳いだら"),
            "話す" to listOf("話した", "話して", "話さない", "話します", "話せる", "話そう", "話せば", "話し", "話しちゃう"),
            "待つ" to listOf("待った", "待って", "待たない", "待ちます", "待てる", "待とう", "待てば"),
            "死ぬ" to listOf("死んだ", "死んで", "死なない", "死にます", "死ねる", "死のう", "死んじゃう"),
            "遊ぶ" to listOf("遊んだ", "遊んで", "遊ばない", "遊びます", "遊べる", "遊ぼう", "遊んだり"),
            "読む" to listOf("読んだ", "読んで", "読まない", "読みます", "読める", "読もう", "読まれる", "読ませる", "読まされる", "読んでいる", "読んでた", "読まず"),
            "取る" to listOf("取った", "取って", "取らない", "取ります", "取れる", "取ろう", "取れば", "取り", "取られた")
        )
        for ((term, forms) in cases) {
            for (form in forms) assertReaches(form, term, v5)
        }
        // 行く is the godan verb in く that takes って.
        assertReaches("行った", "行く", v5)
        assertReaches("行って", "行く", v5)
        assertReaches("行っちゃった", "行く", v5)
    }

    @Test
    fun irregularVerbs() {
        for (form in listOf("した", "して", "しない", "します", "しよう", "すれば", "される", "させる", "している", "しなかった", "せず", "しちゃった", "したい")) {
            assertReaches("勉強$form", "勉強する", WordClass.VS)
        }
        for (form in listOf("きた", "きて", "こない", "きます", "こよう", "くれば", "こられる", "こさせる", "きている", "こなかった")) {
            assertReaches(form, "くる", WordClass.VK)
        }
        for (form in listOf("来た", "来て", "来ない", "来ます", "来よう", "来れば", "来られる")) {
            assertReaches(form, "来る", WordClass.VK)
        }
    }

    @Test
    fun adjectives() {
        val adj = WordClass.ADJ_I
        for (form in listOf("高かった", "高くない", "高くて", "高く", "高ければ", "高さ", "高そう", "高すぎる", "高くなかった", "高かったら", "高かろう")) {
            assertReaches(form, "高い", adj)
        }
        assertReaches("悲しげ", "悲しい", adj)
        assertReaches("知らねえ", "知る", WordClass.V5)
    }

    @Test
    fun reasonsAreListedOutermostFirst() {
        val result = deinflector.deinflect("読まれなかった")
            .first { it.term == "読む" && it.conditions == WordClass.V5 }
        assertEquals(listOf("past", "negative", "passive"), result.reasons)
        // When two chains reach the same word, the shorter one is reported.
        val shortest = deinflector.deinflect("食べさせられた")
            .first { it.term == "食べる" && it.conditions == WordClass.V1 }
        assertEquals(listOf("past", "causative passive"), shortest.reasons)
    }

    @Test
    fun wordClassKeepsUnrelatedFormsApart() {
        // 食べた is not a godan form of anything: no v5 candidate may be called 食べる.
        assertFalse(reaches("食べた", "食べる", WordClass.V5))
        // The te-form marker is internal and never a class a dictionary entry could have.
        assertTrue(deinflector.deinflect("読んでいる").none { it.term == "読む" && it.conditions == WordClass.TE })
    }

    @Test
    fun staysBoundedOnLongInput() {
        val results = deinflector.deinflect("させられなかったらしくてならなかった")
        assertTrue(results.size <= 400)
    }

    @Test
    fun parsesDictionaryRules() {
        assertEquals(WordClass.V1, WordClass.parse("v1"))
        assertEquals(WordClass.V5, WordClass.parse("v5"))
        assertEquals(WordClass.V1 or WordClass.VS, WordClass.parse("v1 vs"))
        assertEquals(WordClass.ADJ_I, WordClass.parse("adj-i"))
        assertEquals(0, WordClass.parse(""))
        assertEquals(0, WordClass.parse("n"))
    }
}
