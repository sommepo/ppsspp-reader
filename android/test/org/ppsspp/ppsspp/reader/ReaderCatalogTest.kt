// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import android.graphics.Color
import android.widget.FrameLayout
import android.widget.ImageView
import androidx.test.core.app.ApplicationProvider
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.ppsspp.ppsspp.reader.ui.ReaderBadge

/**
 * Which games the reader can read, and their mark in the game list: from what each text profile
 * says of itself in its first lines.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = "w768dp-h432dp-400dpi")
class ReaderCatalogTest {
    private val context: Context get() = ApplicationProvider.getApplicationContext()
    private val aceAttorney = "ULJS00219"
    private val unknown = "ULJM09999"

    @Before
    fun setUp() = ReaderCatalog.forget()

    @After
    fun tearDown() = ReaderCatalog.forget()

    private fun ownProfile(titleId: String, source: String): File {
        val folder = File(context.getExternalFilesDir(null), "reader/profiles").apply { mkdirs() }
        return File(folder, "$titleId.lua").apply { writeText(source) }
    }

    @Test
    fun aProfileSaysWhatItIsInItsFirstLines() {
        val info = ReaderCatalog.describe(
            unknown,
            """
            -- reader: no hooks
            -- reader: title 作り話 (A Made-up Game)
            --   reader:   reads   Dialogue and choices
            -- reader: version Base game, version 1
            -- reader: something a later reader may know
            -- An ordinary comment about how the game keeps its text.

            local window = 0x1234
            -- reader: title Not this: the comments at the top have ended
            """.trimIndent(),
            own = false
        )
        assertEquals("作り話 (A Made-up Game)", info.title)
        assertEquals("Dialogue and choices", info.reads)
        assertEquals("Base game, version 1", info.version)
        assertFalse(info.own)

        // A profile that says nothing is known by its ID.
        val silent = ReaderCatalog.describe(unknown, "-- A profile of my own.\nlocal x = 1\n", own = true)
        assertEquals(unknown, silent.title)
        assertEquals("", silent.reads)
        assertEquals("", silent.version)
        assertTrue(silent.own)
    }

    @Test
    fun everyBundledProfileSaysItsGameWhatIsReadAndForWhichVersion() {
        val games = ReaderCatalog.games(context)
        assertEquals(
            setOf("ULJS00219", "ULJM05887", "ULJM05821", "ULJM05718", "ULJM05719"),
            games.map { it.titleId }.toSet()
        )
        for (game in games) {
            assertTrue("${game.titleId} has no title line", game.title != game.titleId && game.title.isNotBlank())
            assertTrue("${game.titleId} does not say what is read", game.reads.isNotBlank())
            assertTrue("${game.titleId} does not say its version", game.version.isNotBlank())
            assertFalse(game.own)
        }
        assertEquals("by title", games.map { it.title }.sortedWith(String.CASE_INSENSITIVE_ORDER), games.map { it.title })
        val ace = games.first { it.titleId == aceAttorney }
        assertEquals("428 ～封鎖された渋谷で～ (428: Shibuya Scramble)", ace.title)
    }

    @Test
    fun gamesWithoutVoicesGetNoAudioOnTheirCardsUnlessTheUserSaysOtherwise() {
        val never = ReaderSettings.CardAudio.NEVER
        // 428: its lines have no voices.
        val withoutVoices = setOf("ULJS00219")
        for (game in ReaderCatalog.games(context)) {
            val said = ReaderCatalog.cardAudio(context, game.titleId)
            assertEquals(game.titleId, if (game.titleId in withoutVoices) never else null, said)
        }
        assertEquals(never, ReaderCatalog.cardAudio(context, aceAttorney.lowercase()))
        val settings = ReaderSettings(context)
        // The profile's word goes before the general choice and after the user's own for the game.
        assertEquals(ReaderSettings.CardAudio.ASK, settings.cardAudioFor("ULJM05000", null))
        assertEquals(never, settings.cardAudioFor(aceAttorney, never))
        settings.setCardAudioOf(aceAttorney, ReaderSettings.CardAudio.ALWAYS)
        assertEquals(ReaderSettings.CardAudio.ALWAYS, settings.cardAudioFor(aceAttorney, never))
        settings.setCardAudioOf(aceAttorney, null)
        // A profile of the user's own for a game says it the same way.
        ownProfile("ULJM05000", "-- reader: title X\n-- reader: audio Never\n")
        ReaderCatalog.forget()
        assertEquals(never, ReaderCatalog.cardAudio(context, "ULJM05000"))
    }

    @Test
    fun aGameIsCoveredWhenItHasAProfileBundledOrTheUsersOwn() {
        assertTrue(ReaderCatalog.covers(context, aceAttorney))
        assertTrue("however the ID is written", ReaderCatalog.covers(context, aceAttorney.lowercase()))
        assertFalse(ReaderCatalog.covers(context, unknown))
        assertFalse("a game without a title ID", ReaderCatalog.covers(context, "0"))
        assertNull(ReaderCatalog.titleIdOf("$aceAttorney.table.bin"))
        assertNull(ReaderCatalog.titleIdOf("notes.lua"))

        ownProfile(unknown, "-- reader: title 作り話\nlocal x = 1\n")
        ownProfile(aceAttorney, "-- reader: title My Ace Attorney profile\nlocal x = 1\n")
        ReaderCatalog.forget()
        assertTrue(ReaderCatalog.covers(context, unknown))
        val games = ReaderCatalog.games(context)
        assertEquals(6, games.size)
        assertTrue(games.first { it.titleId == unknown }.own)
        // The user's profile of a bundled game is the one that is used, so it is the one listed.
        val ace = games.single { it.titleId == aceAttorney }
        assertEquals("My Ace Attorney profile", ace.title)
        assertTrue(ace.own)
    }

    @Test
    fun aReadableGameCarriesTheBadgeInTheGameListAndAnotherDoesNot() {
        val frame = FrameLayout(context)
        val cover = ImageView(context).apply { setBackgroundColor(Color.rgb(0x5B, 0x6E, 0x8C)) }
        frame.addView(cover, FrameLayout.LayoutParams(375, 375))

        ReaderBadge.mark(cover, aceAttorney)
        assertTrue(ReaderBadge.isOn(cover))
        TestSupport.snapshot(frame, 375, 375, "game-list-badge")

        // A row of the list is used again for another game: the badge goes with the game.
        ReaderBadge.mark(cover, unknown)
        assertFalse(ReaderBadge.isOn(cover))
        ReaderBadge.mark(cover, aceAttorney)
        ReaderBadge.mark(cover, aceAttorney)
        assertTrue(ReaderBadge.isOn(cover))
    }
}
