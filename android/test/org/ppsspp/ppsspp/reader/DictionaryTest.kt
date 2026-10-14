// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.ppsspp.ppsspp.reader.dict.DictionaryDatabase
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryLookup
import org.ppsspp.ppsspp.reader.dict.Frequency
import org.ppsspp.ppsspp.reader.dict.GlossaryRenderer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

/** Dictionary import and lookup against small archives written for the test (no real data). */
@RunWith(RobolectricTestRunner::class)
class DictionaryTest {
    @get:Rule
    val folder = TemporaryFolder()

    private lateinit var db: DictionaryDatabase
    private lateinit var importer: DictionaryImporter
    private lateinit var lookup: DictionaryLookup

    private val structured = """{"type":"structured-content","content":[
        {"tag":"div","style":{"fontWeight":"bold"},"data":{"content":"gloss"},"content":"food"},
        {"tag":"ul","content":[{"tag":"li","content":"item one"},
          {"tag":"li","content":["item ",{"tag":"ruby","content":["二",{"tag":"rt","content":"に"}]}]}]},
        {"tag":"img","path":"x.png"}]}"""

    private val terms = """[
        ["食べる","たべる","v1","v1",100,["to eat","to live on"],1,""],
        ["読む","よむ","v5","v5",90,["to read"],2,""],
        ["高い","たかい","adj-i","adj-i",80,["high","expensive"],3,""],
        ["これ","","pn","",70,["this"],4,""],
        ["食べ物","たべもの","n","",60,[$structured],5,""],
        ["名","な","n","",50,["name"],6,""],
        ["名乗る","なのる","v5","v5",50,["to give one's name"],7,""],
        ["マリア","","n","",1,["Maria <the name>"],8,""],
        ["飼う","かう","v5","v5",40,["to keep (a pet)"],9,""],
        ["買う","かう","v5","v5",95,["to buy"],10,""]
    ]"""

    private fun archive(name: String, files: Map<String, String>): File {
        val file = File(folder.root, name) // written afresh each time it is asked for
        ZipOutputStream(file.outputStream()).use { zip ->
            for ((path, content) in files) {
                zip.putNextEntry(ZipEntry(path))
                zip.write(content.toByteArray(Charsets.UTF_8))
                zip.closeEntry()
            }
        }
        return file
    }

    private fun mainArchive() = archive(
        "main.zip",
        mapOf(
            // Deliberately not first in the archive: the importer must not rely on entry order.
            "term_bank_1.json" to terms,
            "index.json" to """{"title":"Test Dict","revision":"r1","format":3,"author":"tests"}""",
            "tag_bank_1.json" to """[["n","partOfSpeech",0,"noun",0]]"""
        )
    )

    private fun frequencyArchive() = archive(
        "freq.zip",
        mapOf(
            "index.json" to """{"title":"Freq","revision":"1","format":3,"frequencyMode":"rank-based"}""",
            "term_meta_bank_1.json" to """[
                ["食べる","freq",1500],
                ["読む","freq",{"reading":"よむ","frequency":{"value":800,"displayValue":"800㋕"}}],
                ["読む","freq",{"reading":"どく","frequency":5}],
                ["読む","pitch",{"reading":"よむ","pitches":[{"position":1},{"position":0}]}],
                ["買う","freq","rank 200"]
            ]"""
        )
    )

    /** A monolingual-style dictionary: no word classes at all. */
    private fun monoArchive() = archive(
        "mono.zip",
        mapOf(
            "index.json" to """{"title":"Mono","revision":"1","format":3}""",
            "term_bank_1.json" to """[["食べる","たべる","","",0,["食物を口に入れる。"],0,""],
                ["た","","","",0,["助動詞。"],0,""]]"""
        )
    )

    @Before
    fun setUp() {
        db = DictionaryDatabase(ApplicationProvider.getApplicationContext<Context>(), null)
        importer = DictionaryImporter(db)
        lookup = DictionaryLookup(db)
    }

    @After
    fun tearDown() {
        db.close()
    }

    @Test
    fun importsTermsAndReportsProgress() {
        val steps = ArrayList<Pair<Int, Int>>()
        val result = importer.import(mainArchive(), { done, total, _ -> steps.add(done to total) })
        assertEquals("Test Dict", result.title)
        assertEquals(10, result.terms)
        assertEquals(0, result.metas)
        assertEquals(listOf(0 to 1, 1 to 1), steps)
        val info = db.dictionaries().single()
        assertEquals("Test Dict", info.title)
        assertEquals("r1", info.revision)
        assertEquals(10, info.termCount)
        assertTrue(info.enabled)
    }

    @Test
    fun refusesDuplicatesAndBrokenArchives() {
        importer.import(mainArchive())
        val again = assertThrows(DictionaryImporter.ImportException::class.java) {
            importer.import(mainArchive())
        }
        assertTrue(again.message!!.contains("already installed"))
        assertThrows(DictionaryImporter.ImportException::class.java) {
            importer.import(archive("empty.zip", mapOf("readme.txt" to "hello")))
        }
        assertThrows(DictionaryImporter.ImportException::class.java) {
            importer.import(archive("noterms.zip", mapOf("index.json" to """{"title":"X","format":3}""")))
        }
        // A bank that breaks half way leaves nothing behind.
        assertThrows(DictionaryImporter.ImportException::class.java) {
            importer.import(
                archive(
                    "broken.zip",
                    mapOf(
                        "index.json" to """{"title":"Broken","format":3}""",
                        "term_bank_1.json" to """[["良い","よい","","adj-i",1,["good"],1,""],["oops"""
                    )
                )
            )
        }
        assertEquals(listOf("Test Dict"), db.dictionaries().map { it.title })
        assertTrue(lookup.lookup("良い").isEmpty())
    }

    @Test
    fun cancellingAnImportLeavesTheDatabaseUnchanged() {
        assertThrows(DictionaryImporter.ImportException::class.java) {
            importer.import(mainArchive(), keepGoing = { false })
        }
        assertTrue(db.dictionaries().isEmpty())
    }

    @Test
    fun findsInflectedWordsAndSaysWhy() {
        importer.import(mainArchive())
        val past = lookup.lookup("食べた。それから")
        assertEquals("食べる", past[0].expression)
        assertEquals("たべる", past[0].reading)
        assertEquals("食べた", past[0].matchedText)
        assertEquals(listOf("past"), past[0].reasons)

        val progressive = lookup.lookup("読んでいる")
        assertEquals("読む", progressive[0].expression)
        assertEquals("読んでいる", progressive[0].matchedText)
        assertEquals(listOf("progressive or perfect", "-te"), progressive[0].reasons)

        val imperative = lookup.lookup("名乗れ")
        assertEquals(listOf("名乗る", "名"), imperative.map { it.expression })
        assertEquals("名乗れ", imperative[0].matchedText)
        assertEquals("名", imperative[1].matchedText)
    }

    @Test
    fun prefersTheLongestMatch() {
        importer.import(mainArchive())
        val results = lookup.lookup("食べ物を買う")
        assertEquals("食べ物", results[0].expression)
        // The shorter reading of the same text is still offered after it.
        assertTrue(results.any { it.expression == "食べる" && it.matchedText == "食べ" })
    }

    @Test
    fun matchesKanaByReadingAndKatakanaAsWritten() {
        importer.import(mainArchive())
        val byReading = lookup.lookup("たかくない")
        assertEquals("高い", byReading[0].expression)
        assertEquals("たかくない", byReading[0].matchedText)
        // Two words share the reading かう: both are offered, the higher score first.
        assertEquals(listOf("買う", "飼う"), lookup.lookup("かった").map { it.expression })
        assertEquals("マリア", lookup.lookup("マリアだ")[0].expression)
        assertEquals("", lookup.lookup("マリアだ")[0].displayReading)
        assertEquals("これ", lookup.lookup("これは")[0].expression)
    }

    @Test
    fun stopsAtPunctuationAndFindsNothingInNonWords() {
        importer.import(mainArchive())
        assertTrue(lookup.lookup("").isEmpty())
        assertTrue(lookup.lookup("、食べる").isEmpty())
        assertTrue(lookup.lookup("…").isEmpty())
        // 食べる。読む: the scan must not run across the full stop.
        assertEquals("食べる", lookup.lookup("食べる。読む")[0].matchedText)
    }

    @Test
    fun wordClassRulesOutFalseDeinflections() {
        importer.import(mainArchive())
        // 名た would be a "past" of an ichidan 名る; 名 is a noun, so only the plain 名 matches.
        val results = lookup.lookup("名た")
        assertEquals(listOf("名"), results.map { it.expression })
        assertEquals("名", results[0].matchedText)
        assertTrue(results[0].reasons.isEmpty())
    }

    @Test
    fun dictionariesWithoutWordClassesJoinConfirmedHeadwords() {
        importer.import(mainArchive())
        importer.import(monoArchive())
        val results = lookup.lookup("食べた")
        assertEquals("食べる", results[0].expression)
        assertEquals(listOf("Test Dict", "Mono"), results[0].entries.map { it.dictionary })
        // On its own, the class-less dictionary cannot confirm an inflection...
        db.setEnabled(db.dictionaries().first { it.title == "Test Dict" }.id, false)
        assertTrue(lookup.lookup("食べた").none { it.expression == "食べる" })
        // ...but still matches what is written.
        assertEquals("食べる", lookup.lookup("食べる")[0].expression)
    }

    @Test
    fun attachesFrequenciesAndPitchForTheRightReading() {
        importer.import(mainArchive())
        importer.import(frequencyArchive())
        val yomu = lookup.lookup("読む")[0]
        assertEquals(listOf(Frequency("Freq", "800㋕", 800.0, true)), yomu.frequencies)
        assertEquals(listOf(1, 0), yomu.pitches.single().positions)
        val taberu = lookup.lookup("食べる")[0]
        assertEquals(listOf(Frequency("Freq", "1500", 1500.0, true)), taberu.frequencies)
        assertTrue(taberu.pitches.isEmpty())
        assertEquals(listOf(Frequency("Freq", "rank 200", null, true)), lookup.lookup("買う")[0].frequencies)
        assertEquals(1500L, DictionaryLookup.harmonicRank(taberu.frequencies))
        assertNull(DictionaryLookup.harmonicRank(lookup.lookup("買う")[0].frequencies))
        assertEquals(
            1043L,
            DictionaryLookup.harmonicRank(listOf(Frequency("a", "800", 800.0, true), Frequency("b", "1500", 1500.0, true)))
        )
    }

    @Test
    fun orderEnablingAndDeletionAreRespected() {
        importer.import(mainArchive())
        importer.import(monoArchive())
        val (main, mono) = db.dictionaries().let { it[0] to it[1] }
        db.setOrder(listOf(mono.id, main.id))
        assertEquals(listOf("Mono", "Test Dict"), db.dictionaries().map { it.title })
        assertEquals(listOf("Mono", "Test Dict"), lookup.lookup("食べる")[0].entries.map { it.dictionary })
        db.setEnabled(mono.id, false)
        assertEquals(listOf("Test Dict"), lookup.lookup("食べる")[0].entries.map { it.dictionary })
        assertTrue(db.hasEnabledDictionaries())
        db.delete(main.id)
        assertTrue(lookup.lookup("食べる").isEmpty())
        assertFalse(db.hasEnabledDictionaries())
        assertEquals(listOf("Mono"), db.dictionaries().map { it.title })
    }

    @Test
    fun rendersGlossariesForCardsAndForThePopup() {
        importer.import(mainArchive())
        val plain = lookup.lookup("食べる")[0].entries[0].glossaryJson
        assertEquals(listOf("to eat", "to live on"), GlossaryRenderer.definitions(plain, GlossaryRenderer.Mode.FULL))
        assertEquals("<ul><li>to eat</li><li>to live on</li></ul>", GlossaryRenderer.toHtml(plain, GlossaryRenderer.Mode.FULL))
        assertEquals("• to eat<br>• to live on", GlossaryRenderer.toHtml(plain, GlossaryRenderer.Mode.SIMPLE))
        assertEquals("to eat; to live on", GlossaryRenderer.toPlainText(plain))

        val rich = lookup.lookup("食べ物")[0].entries[0].glossaryJson
        assertEquals(
            "<div style=\"font-weight: bold;\" data-sc-content=\"gloss\">food</div>" +
                "<ul><li>item one</li><li>item <ruby>二<rt>に</rt></ruby></li></ul>",
            GlossaryRenderer.toHtml(rich, GlossaryRenderer.Mode.FULL)
        )
        assertEquals(
            "<b>food</b><br>• item one<br>• item 二",
            GlossaryRenderer.toHtml(rich, GlossaryRenderer.Mode.SIMPLE)
        )
        // Text from a dictionary is escaped, never passed through as markup.
        val name = lookup.lookup("マリア")[0].entries[0].glossaryJson
        assertEquals("Maria &lt;the name&gt;", GlossaryRenderer.toHtml(name, GlossaryRenderer.Mode.FULL))
        assertEquals("Maria <the name>", GlossaryRenderer.toPlainText(name))
    }

    @Test
    fun glossariesSurviveCompression() {
        val long = "[\"" + "定義".repeat(400) + "\"]"
        val short = "[\"a\"]"
        for (json in listOf(long, short, "[]")) {
            assertEquals(json, DictionaryDatabase.unpack(DictionaryDatabase.pack(json)))
        }
        assertTrue(DictionaryDatabase.pack(long).size < long.toByteArray().size / 4)
    }
}
