// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.io.File
import java.security.MessageDigest
import org.ppsspp.ppsspp.reader.anki.CardBuilder
import org.ppsspp.ppsspp.reader.mining.WordAudio
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The word's own sound on a card: where it is asked for, what counts as a recording, and the
 * marker and fields it goes into. Nothing here reaches the network: the fetcher is a stand-in.
 */
class WordAudioTest {
    private val real = WordAudio.fetcher

    @After
    fun tearDown() {
        WordAudio.fetcher = real
    }

    @Test
    fun aWordIsAskedForByItsWritingAndItsReading() {
        assertEquals(
            "https://assets.languagepod101.com/dictionary/japanese/audiomp3.php" +
                "?kanji=%E4%BD%9C%E3%82%8A%E8%A9%B1&kana=%E3%81%A4%E3%81%8F%E3%82%8A%E3%81%B0%E3%81%AA%E3%81%97",
            WordAudio.address("作り話", "つくりばなし")
        )
        // A word in kana alone, and one whose reading is not known, by what there is.
        assertTrue(WordAudio.address("はなし", "はなし").endsWith("audiomp3.php?kana=%E3%81%AF%E3%81%AA%E3%81%97"))
        assertTrue(WordAudio.address("ハナシ", "").endsWith("audiomp3.php?kana=%E3%83%8F%E3%83%8A%E3%82%B7"))
    }

    @Test
    fun theRecordingThatSaysThereIsNoSuchWordIsNotARecordingOfAWord() {
        val recording = ByteArray(3000) { (it % 251).toByte() }
        assertTrue(WordAudio.isRecording(recording))
        assertFalse(WordAudio.isRecording(null))
        assertFalse("too short to be sound", WordAudio.isRecording(ByteArray(40)))
        // The fingerprint is the one the dictionary's own "no such word" recording has; what
        // has it is told here by a fetcher that answers with bytes of that fingerprint.
        assertEquals(64, WordAudio.MISSING.length)
        val target = File.createTempFile("word", ".mp3")
        try {
            var asked = ""
            WordAudio.fetcher = { address -> asked = address; recording }
            assertTrue(WordAudio.fetch("作り話", "つくりばなし", target))
            assertEquals(WordAudio.address("作り話", "つくりばなし"), asked)
            assertTrue(target.readBytes().contentEquals(recording))
            // Nothing to be had, or the server not reached: no recording, and no failure.
            WordAudio.fetcher = { null }
            assertFalse(WordAudio.fetch("作り話", "つくりばなし", target))
            WordAudio.fetcher = { throw java.io.IOException("no network") }
            assertFalse(WordAudio.fetch("作り話", "つくりばなし", target))
            assertFalse("no word, nothing asked", WordAudio.fetch("", "", target))
        } finally {
            target.delete()
        }
        val sha = MessageDigest.getInstance("SHA-256").digest(recording).joinToString("") { "%02x".format(it) }
        assertFalse(sha == WordAudio.MISSING)
    }

    @Test
    fun theSameWordIsKeptUnderTheSameName() {
        val name = WordAudio.fileName("作り話", "つくりばなし")
        assertTrue(name.matches(Regex("reader_word_[0-9a-f]{16}\\.mp3")))
        assertEquals(name, WordAudio.fileName("作り話", "つくりばなし"))
        assertFalse(name == WordAudio.fileName("作り話", "つくりはなし"))
    }

    @Test
    fun aFieldForTheWordsSoundGetsTheMarker() {
        val guessed = CardBuilder.suggestMapping(listOf("Word", "WordAudio", "SentenceAudio", "Audio", "Notes"))
        assertEquals("{word-audio}", guessed["WordAudio"])
        assertEquals("{word-audio}", guessed["Audio"])
        assertEquals("{audio}", guessed["SentenceAudio"])
        assertEquals("", guessed["Notes"])
        assertTrue(CardBuilder.needsWordAudio(guessed.values))
        assertFalse(CardBuilder.needsWordAudio(listOf("{audio}", "{expression}")))
        assertTrue(CardBuilder.MARKERS.any { it.first == "word-audio" })
    }

    @Test
    fun anOlderMappingGetsTheMarkerWhereItLeftTheWordsSoundEmpty() {
        val older = mapOf("Word" to "{expression}", "WordAudio" to "", "SentenceAudio" to "{audio}", "Hint" to "")
        val offered = CardBuilder.offerWordAudio(older)
        assertEquals("{word-audio}", offered["WordAudio"])
        assertEquals("", offered["Hint"])
        assertEquals("{audio}", offered["SentenceAudio"])
        // What somebody put there stays.
        assertEquals("mine", CardBuilder.offerWordAudio(mapOf("WordAudio" to "mine"))["WordAudio"])
    }
}
