// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.dict.JapaneseText
import org.ppsspp.ppsspp.reader.dict.JapaneseText.FuriganaSegment
import org.junit.Assert.assertEquals
import org.junit.Test

class JapaneseTextTest {
    @Test
    fun convertsBetweenKanaScripts() {
        assertEquals("こーひー", JapaneseText.katakanaToHiragana("コーヒー"))
        assertEquals("タベル", JapaneseText.hiraganaToKatakana("たべる"))
        assertEquals("漢字かな", JapaneseText.katakanaToHiragana("漢字カナ"))
        assertEquals("ABC123", JapaneseText.fullWidthToAscii("ＡＢＣ１２３"))
    }

    @Test
    fun distributesFuriganaOverKanjiOnly() {
        assertEquals(
            listOf(FuriganaSegment("食", "た"), FuriganaSegment("べ", null), FuriganaSegment("物", "もの")),
            JapaneseText.distributeFurigana("食べ物", "たべもの")
        )
        assertEquals(
            listOf(FuriganaSegment("お", null), FuriganaSegment("茶", "ちゃ")),
            JapaneseText.distributeFurigana("お茶", "おちゃ")
        )
        assertEquals(
            listOf(FuriganaSegment("取", "と"), FuriganaSegment("り", null), FuriganaSegment("引", "ひ"), FuriganaSegment("き", null)),
            JapaneseText.distributeFurigana("取り引き", "とりひき")
        )
        // No kana to anchor on: the whole reading sits over the whole word.
        assertEquals(listOf(FuriganaSegment("東京", "とうきょう")), JapaneseText.distributeFurigana("東京", "とうきょう"))
        // Kana words have nothing to annotate.
        assertEquals(listOf(FuriganaSegment("これ", null)), JapaneseText.distributeFurigana("これ", ""))
        assertEquals(listOf(FuriganaSegment("コーヒー", null)), JapaneseText.distributeFurigana("コーヒー", "こーひー"))
        // A reading that does not fit the kana falls back to one segment.
        assertEquals(listOf(FuriganaSegment("食べる", "のむ")), JapaneseText.distributeFurigana("食べる", "のむ"))
    }

    @Test
    fun formatsFuriganaForAnki() {
        assertEquals("食[た]べ 物[もの]", JapaneseText.ankiFurigana("食べ物", "たべもの"))
        assertEquals("お 茶[ちゃ]", JapaneseText.ankiFurigana("お茶", "おちゃ"))
        assertEquals("これ", JapaneseText.ankiFurigana("これ", ""))
        assertEquals("<ruby>食<rt>た</rt></ruby>べる", JapaneseText.rubyHtml("食べる", "たべる"))
    }
}
