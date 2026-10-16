// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceGroup
import androidx.preference.PreferenceManager
import androidx.preference.SeekBarPreference
import androidx.preference.SwitchPreferenceCompat
import androidx.recyclerview.widget.RecyclerView
import androidx.test.core.app.ApplicationProvider
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.materialswitch.MaterialSwitch
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.anki.CardBuilder
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryStore
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.ReadingRules
import org.ppsspp.ppsspp.reader.ui.AnkiSetupActivity
import org.ppsspp.ppsspp.reader.ui.DictionaryManagerActivity
import org.ppsspp.ppsspp.reader.ui.LookupPopupView
import org.ppsspp.ppsspp.reader.ui.ReaderBacklogActivity
import org.ppsspp.ppsspp.reader.ui.ReaderGamesActivity
import org.ppsspp.ppsspp.reader.ui.ReaderSettingsActivity
import org.ppsspp.ppsspp.reader.ui.ReaderStatsActivity
import org.ppsspp.ppsspp.reader.ui.ReaderTextView
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog

/**
 * The reader's own screens, started as real activities off-device with data seeded for the
 * test: each must come up, show what is stored and carry out its actions. PNGs are written
 * when READER_PREVIEW_DIR is set.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = "w768dp-h432dp-400dpi")
class ReaderScreensTest {
    private val context: Context get() = ApplicationProvider.getApplicationContext()
    private val game = "0004000000088A00"
    private var stub: AnkiConnectStub? = null

    @Before
    fun setUp() {
        TestSupport.installApplication()
        TestSupport.resetStores()
    }

    @After
    fun tearDown() {
        stub?.close()
        TestSupport.resetStores()
    }

    private fun root(activity: Activity): View = activity.findViewById(android.R.id.content)

    private fun text(activity: Activity) = TestSupport.allText(root(activity))

    private fun button(activity: Activity, label: String): View =
        TestSupport.find(root(activity), TextView::class.java) { it.text.toString() == label }
            ?: throw AssertionError("no \"$label\" on screen:\n${text(activity)}")

    private fun confirmDialog() {
        val dialog = ShadowDialog.getLatestDialog() as AlertDialog
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick()
        TestSupport.idle()
    }

    private fun installDictionary(title: String = "Test Dict") {
        val zip = File(context.cacheDir, "$title.zip")
        ZipOutputStream(zip.outputStream()).use { out ->
            out.putNextEntry(ZipEntry("index.json"))
            out.write("""{"title":"$title","revision":"2026-10","format":3}""".toByteArray())
            out.closeEntry()
            out.putNextEntry(ZipEntry("term_bank_1.json"))
            out.write(
                """[["見張る","みはる","v5","v5",10,["to keep watch"],1,""],
                    ["道","みち","n","",5,["road"],2,""]]""".toByteArray()
            )
            out.closeEntry()
        }
        DictionaryImporter(DictionaryStore.get(context).database).import(zip)
    }

    @Test
    fun statisticsShowTotalsAndDeleteASession() {
        val db = ReaderSession.database(context)
        val today = ReadingRules.readingDay(System.currentTimeMillis())
        db.setGameTitle(game, "真・女神転生IV")
        val older = db.createSession(game, 1_758_000_000_000)
        db.addActivity(older, game, "2026-09-16", ReadingCounters(1_800_000, 6000, 5500, 500, 210, 40), 1_758_003_600_000)
        db.closeSession(older, 1_758_003_600_000)
        val current = db.createSession(game, System.currentTimeMillis() - 600_000)
        db.addActivity(current, game, today, ReadingCounters(600_000, 2468, 2468, 0, 80, 0), System.currentTimeMillis())

        val activity = Robolectric.buildActivity(ReaderStatsActivity::class.java).setup().get()
        TestSupport.await("statistics") { text(activity).contains("真・女神転生IV") }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-statistics")
        val shown = text(activity)
        assertTrue(shown, shown.contains("2,468")) // today
        assertTrue(shown, shown.contains("8,468")) // all games
        assertTrue(shown, shown.contains("40m 00s")) // 30 + 10 minutes
        assertTrue(shown, shown.contains("12,702")) // 8468 characters in 40 minutes, per hour
        assertTrue(shown, shown.contains("09-16"))

        // Deleting the older session removes exactly its share.
        val row = TestSupport.find(root(activity), TextView::class.java) { it.text.startsWith("2025-09-16") || it.text.startsWith("2026-09-16") }
        assertNotNull(shown, row)
        (row!!.parent as View).performClick()
        confirmDialog()
        TestSupport.await("totals after deletion") { !text(activity).contains("8,468") }
        assertEquals(2468L, db.totals(game).characters)
        assertEquals(listOf(current), db.sessions(game).map { it.id })
    }

    @Test
    fun statisticsSayWhenThereIsNothingYet() {
        val activity = Robolectric.buildActivity(ReaderStatsActivity::class.java).setup().get()
        TestSupport.await("empty message") { text(activity).contains("No reading recorded yet.") }
    }

    @Test
    fun dictionaryManagerListsReordersAndDeletes() {
        installDictionary("Test Dict")
        installDictionary("Second Dict")
        val store = DictionaryStore.get(context)
        val activity = Robolectric.buildActivity(DictionaryManagerActivity::class.java).setup().get()
        TestSupport.await("dictionary list") { text(activity).contains("Second Dict") }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-dictionaries")
        assertTrue(text(activity).contains("2 terms"))
        assertTrue(text(activity).contains("2026-10"))

        // Switch the first one off.
        TestSupport.find(root(activity), MaterialSwitch::class.java)!!.performClick()
        TestSupport.await("switch stored") { !store.database.dictionaries().first { it.title == "Test Dict" }.enabled }

        // Move the second above the first.
        val up = ArrayList<View>()
        fun collect(v: View) {
            if (v.contentDescription == "Move up") up.add(v)
            if (v is android.view.ViewGroup) for (i in 0 until v.childCount) collect(v.getChildAt(i))
        }
        collect(root(activity))
        assertFalse(up[0].isEnabled)
        up[1].performClick()
        TestSupport.await("new order") { store.database.dictionaries().map { it.title } == listOf("Second Dict", "Test Dict") }

        // Delete asks first.
        TestSupport.await("list redrawn") {
            TestSupport.find(root(activity), TextView::class.java)?.let { text(activity).indexOf("Second Dict") < text(activity).indexOf("Test Dict") } == true
        }
        TestSupport.find(root(activity), View::class.java) { it.contentDescription == "Delete" }!!.performClick()
        confirmDialog()
        TestSupport.await("deletion") { store.database.dictionaries().map { it.title } == listOf("Test Dict") }
    }

    @Test
    fun theSetupScreensReaderPagesKnowWhenTheyAreDone() {
        // A dictionary: one is installed.
        assertFalse(ReaderSetup.hasDictionary(context))
        installDictionary("Test Dict")
        assertTrue(ReaderSetup.hasDictionary(context))

        // Anki: a deck and a note type are chosen, and cards are not switched off.
        val prefs = PreferenceManager.getDefaultSharedPreferences(context)
        assertFalse(ReaderSetup.hasAnki(context))
        prefs.edit().putString(ReaderSettings.KEY_ANKI_DECK, "Mining").commit()
        assertFalse(ReaderSetup.hasAnki(context))
        prefs.edit().putString(ReaderSettings.KEY_ANKI_MODEL, "Lapis").commit()
        assertTrue(ReaderSetup.hasAnki(context))
        prefs.edit().putString(ReaderSettings.KEY_ANKI_TRANSPORT, ReaderSettings.ANKI_NONE).commit()
        assertFalse(ReaderSetup.hasAnki(context))

        // What the pages say fits a page of the setup screen.
        for (id in listOf(
            R.string.reader_setup_sync_description, R.string.reader_setup_dictionaries_description,
            R.string.reader_setup_anki_description
        )) {
            assertTrue(context.getString(id), context.getString(id).length <= 240)
        }
    }

    @Test
    fun dictionaryManagerExplainsAnEmptyList() {
        val activity = Robolectric.buildActivity(DictionaryManagerActivity::class.java).setup().get()
        TestSupport.await("empty hint") { text(activity).contains("No dictionaries yet") }
    }

    @Test
    fun ankiSetupConnectsSuggestsFieldsAndSaves() {
        val server = AnkiConnectStub().also { stub = it }
        val prefs = PreferenceManager.getDefaultSharedPreferences(context)
        prefs.edit()
            .putString(ReaderSettings.KEY_ANKI_TRANSPORT, ReaderSettings.ANKI_ANKICONNECT)
            .putString(ReaderSettings.KEY_ANKICONNECT_URL, server.url)
            .apply()

        val activity = Robolectric.buildActivity(AnkiSetupActivity::class.java).setup().get()
        TestSupport.await("connection") { text(activity).contains("Connected: 2 decks, 2 note types") }
        val spinners = ArrayList<Spinner>()
        fun collect(v: View) {
            if (v is Spinner) spinners.add(v)
            if (v is android.view.ViewGroup) for (i in 0 until v.childCount) collect(v.getChildAt(i))
        }
        collect(root(activity))
        assertEquals(2, spinners.size)
        spinners[0].setSelection(1) // 日本語::Mining
        spinners[1].setSelection(1) // Lapis
        TestSupport.await("fields of the note type") { text(activity).contains("Picture") }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-anki-setup")

        // Fields come pre-filled from their names; one is edited by hand.
        val editors = ArrayList<EditText>()
        fun fields(v: View) {
            if (v is EditText && v.text.toString().startsWith("{")) editors.add(v)
            if (v is android.view.ViewGroup) for (i in 0 until v.childCount) fields(v.getChildAt(i))
        }
        fields(root(activity))
        assertEquals(listOf("{expression}", "{sentence}", "{screenshot}"), editors.map { it.text.toString() })
        editors[1].setText("{sentence}<br>{audio}")

        button(activity, "Save").performClick()
        TestSupport.idle()
        val settings = ReaderSettings(context)
        assertEquals(ReaderSettings.ANKI_ANKICONNECT, settings.ankiTransport)
        assertEquals("日本語::Mining", settings.ankiDeck)
        assertEquals("Lapis", settings.ankiModel)
        assertEquals(
            mapOf("Expression" to "{expression}", "Sentence" to "{sentence}<br>{audio}", "Picture" to "{screenshot}"),
            CardBuilder.parseMapping(settings.ankiFieldMap)
        )
        // The note type's first field is what a word is recognised by in Anki later on.
        assertEquals("Expression", settings.ankiKeyField("Lapis"))
        assertEquals("it belongs to that note type alone", null, settings.ankiKeyField("Basic"))
        assertFalse("a word in another deck is not there, unless asked for", settings.ankiDuplicatesAnywhere)
        assertTrue(activity.isFinishing)
    }

    @Test
    fun ankiSetupSaysWhenAnkiDroidIsMissing() {
        val activity = Robolectric.buildActivity(AnkiSetupActivity::class.java).setup().get()
        TestSupport.await("AnkiDroid status") { text(activity).contains("AnkiDroid is not installed.") }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-anki-no-ankidroid")
    }

    // ---- The reader's page: statistics, the supported games, a page per section ----

    private fun settingsPage(activity: ReaderSettingsActivity): PreferenceFragmentCompat {
        activity.supportFragmentManager.executePendingTransactions()
        TestSupport.idle()
        return activity.supportFragmentManager.findFragmentById(R.id.reader_fragment_container)
            as PreferenceFragmentCompat
    }

    /** Everything on a page in its order; an entry of a group follows the group's heading. */
    private fun entries(group: PreferenceGroup): List<Preference> =
        (0 until group.preferenceCount).map(group::getPreference).flatMap {
            if (it is PreferenceGroup) listOf(it) + entries(it) else listOf(it)
        }

    private fun heading(activity: Activity) =
        TestSupport.find(root(activity), MaterialToolbar::class.java)!!.title.toString()

    @Test
    fun theReadersPageHasStatisticsAndTheGamesFirstThenAnEntryPerSection() {
        val activity = Robolectric.buildActivity(ReaderSettingsActivity::class.java).setup().get()
        val page = settingsPage(activity)
        TestSupport.await("what was read today") { text(activity).contains("Nothing read today") }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-settings")
        assertEquals("Reader", heading(activity))
        assertEquals(
            listOf(
                "Statistics", "Game scripts", "Settings", "Looking words up", "Cursor", "Reader panel",
                "Dictionaries", "Anki and cards", "Statistics options", "Text output", "CRT picture"
            ),
            entries(page.preferenceScreen).map { it.title.toString() }
        )
        assertEquals(
            "5 games whose text can be read",
            page.findPreference<Preference>("reader_open_games")!!.summary.toString()
        )
        // No setting is on this page: each is one tap further, on its section's page.
        assertTrue(entries(page.preferenceScreen).none { it is SwitchPreferenceCompat || it is SeekBarPreference })
    }

    @Test
    fun theReadersPageSaysWhatWasReadToday() {
        val db = ReaderSession.database(context)
        val now = System.currentTimeMillis()
        db.setGameTitle(game, "Made-up Game")
        db.addActivity(
            db.createSession(game, now - 600_000), game, ReadingRules.readingDay(now),
            ReadingCounters(activeMs = 600_000, characters = 1234, uniqueCharacters = 1234, lines = 40), now
        )
        val activity = Robolectric.buildActivity(ReaderSettingsActivity::class.java).setup().get()
        TestSupport.await("what was read today") { text(activity).contains("Today: 1,234字 · 7,400/h · 10m") }
    }

    @Test
    fun aSectionOpensAsAPageOfItsOwnAndBackReturnsToTheFirst() {
        val activity = Robolectric.buildActivity(ReaderSettingsActivity::class.java).setup().get()
        val first = settingsPage(activity)
        val entry = first.findPreference<Preference>("reader_section_cursor")!!
        entry.onPreferenceClickListener!!.onPreferenceClick(entry)

        val cursor = settingsPage(activity)
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-settings-cursor")
        assertEquals(ReaderSettingsActivity.Section.CURSOR, activity.openSection)
        assertEquals("Cursor", heading(activity))
        assertEquals(
            listOf(
                "reader_cursor_button", "reader_cursor_toggle", "reader_cursor_frame",
                "reader_popup_hide_on_exit", "reader_cursor_home",
                "reader_second_screen", "reader_cursor_speed"
            ),
            entries(cursor.preferenceScreen).mapNotNull { it.key }
        )
        assertTrue(text(activity), text(activity).contains("Press once instead of holding"))

        activity.onBackPressedDispatcher.onBackPressed()
        settingsPage(activity)
        assertNull(activity.openSection)
        assertEquals("Reader", heading(activity))
        assertFalse("still open", activity.isFinishing)
    }

    @Test
    fun everySettingIsOnOnePageAndNothingIsExplainedAtLength() {
        val activity = Robolectric.buildActivity(ReaderSettingsActivity::class.java).setup().get()
        val keys = ArrayList<String>()
        val pages = ReaderSettingsActivity.Section.values().map { section ->
            activity.open(section)
            settingsPage(activity).also { page -> keys += entries(page.preferenceScreen).mapNotNull { it.key } }
        } + run {
            repeat(ReaderSettingsActivity.Section.values().size) { activity.onBackPressedDispatcher.onBackPressed() }
            settingsPage(activity)
        }
        assertEquals(
            listOf(
                ReaderSettings.KEY_SCREEN_TEXT, ReaderSettings.KEY_TOUCH_PLAY, ReaderSettings.KEY_POPUP_SCALE,
                ReaderSettings.KEY_POPUP_AUTOHIDE,
                ReaderSettings.KEY_POPUP_HEIGHT, ReaderSettings.KEY_CARD_CORNER,
                ReaderSettings.KEY_WORD_MARK, ReaderSettings.KEY_SCREEN_TEXT_OUTLINE,
                ReaderSettings.KEY_CURSOR_BUTTON, ReaderSettings.KEY_CURSOR_TOGGLE, ReaderSettings.KEY_CURSOR_FRAME,
                ReaderSettings.KEY_POPUP_HIDE_ON_EXIT, ReaderSettings.KEY_CURSOR_HOME, ReaderSettings.KEY_SECOND_SCREEN, ReaderSettings.KEY_CURSOR_SPEED,
                ReaderSettings.KEY_PANEL_ENABLED, ReaderSettings.KEY_PANEL_AUTO_HIDE,
                ReaderSettings.KEY_PANEL_DISPLAY, ReaderSettings.KEY_PANEL_POSITION, ReaderSettings.KEY_FONT_SIZE,
                ReaderSettings.KEY_PANEL_HEIGHT, ReaderSettings.KEY_PANEL_OPACITY, ReaderSettings.KEY_FURIGANA,
                ReaderSettings.KEY_SHOW_CHOICES, ReaderSettings.KEY_REREAD_COLOUR, ReaderSettings.KEY_ADVANCE_BUTTON,
                "reader_open_anki", ReaderSettings.KEY_CARD_AUDIO, ReaderSettings.KEY_CAPTURE_AUDIO,
                ReaderSettings.KEY_AUDIO_MAX, ReaderSettings.KEY_AUDIO_LEAD, ReaderSettings.KEY_SCREENSHOT_CACHE,
                ReaderSettings.KEY_SCREENSHOT_WIDTH,
                ReaderSettings.KEY_SESSION_HUD, ReaderSettings.KEY_STATS_PAUSED, ReaderSettings.KEY_STATS_BUTTON,
                ReaderSettings.KEY_IDLE_MINUTES, "reader_stats_sync_category", ReaderSettings.KEY_STATS_SERVER, ReaderSettings.KEY_STATS_CODE,
                ReaderSettings.KEY_STATS_DEVICE,
                ReaderSettings.KEY_HOOK_ENABLED, ReaderSettings.KEY_HOOK_PORT, ReaderSettings.KEY_HOOK_FORMAT,
                ReaderSettings.KEY_HOOK_SPEAKER, ReaderSettings.KEY_HOOK_LAN, ReaderSettings.KEY_CLIPBOARD_AUTO
            ),
            keys
        )

        // A summary says a thing in a line or two; what needs more belongs in the guide.
        for (page in pages) {
            for (entry in entries(page.preferenceScreen)) {
                val summary = entry.summary?.toString().orEmpty()
                assertTrue("${entry.key ?: entry.title}: $summary", summary.length <= 120)
            }
        }
    }

    @Test
    fun theGameScriptsAreListedAndOneFromThePublishedListIsInstalled() {
        // The published list, served by a stand-in: the bundled script of 428 as it is, and
        // a made-up game's that the app does not have.
        val bundled = context.assets.open("reader/profiles/ULJS00219.lua").use { it.readBytes() }
        val madeUp = "-- reader: no hooks\n-- reader: title 作り話\n-- reader: reads The message window\nlocal x = 1\n"
            .toByteArray()
        fun entry(id: String, title: String, bytes: ByteArray) = org.json.JSONObject()
            .put("id", id).put("title", title).put("reads", "").put("version", "").put("api", 1)
            .put("file", "$id.lua").put("size", bytes.size).put("sha256", ScriptStore.sha256(bytes))
        val list = org.json.JSONObject().put("format", 1).put("system", "ps2").put(
            "scripts", org.json.JSONArray().put(entry("ULJS00219", "428", bundled)).put(entry("ULJM99999", "作り話", madeUp))
        )
        val served = mapOf(
            "https://example.invalid/scripts/index.json" to list.toString().toByteArray(),
            "https://example.invalid/scripts/ULJM99999.lua" to madeUp,
        )
        val fetched = ArrayList<String>()
        ScriptStore.indexUrl = "https://example.invalid/scripts/index.json"
        ScriptStore.fetcher = { url -> fetched.add(url); served[url] ?: throw ScriptStore.ScriptException("404 $url") }
        try {
            val activity = Robolectric.buildActivity(ReaderGamesActivity::class.java).setup().get()
            TestSupport.await("the list") { text(activity).contains("作り話") }
            TestSupport.snapshot(root(activity), 1920, 1080, "screen-games")
            val shown = text(activity)
            assertTrue(shown, shown.contains("Game scripts"))
            assertTrue(shown, shown.contains("2 scripts in the published list."))
            assertTrue(shown, shown.contains("ULJS00219 · Installed"))
            assertTrue(shown, shown.contains("ULJM99999 · Not installed"))
            assertTrue(shown, shown.contains("book badge in the game list"))

            val install = TestSupport.find(root(activity), Button::class.java) { it.text == "Install" }!!
            install.performClick()
            TestSupport.await("installed") { text(activity).contains("Installed the script of 作り話.") && text(activity).contains("ULJM99999 · Installed") }
            val file = File(context.getExternalFilesDir(null), "reader/profiles/ULJM99999.lua")
            assertTrue(file.readBytes().contentEquals(madeUp))
            assertTrue(fetched.contains("https://example.invalid/scripts/ULJM99999.lua"))
            assertTrue(ReaderCatalog.covers(context, "ULJM99999"))

            // The user's own now has a Remove button; removing it leaves the list's to install.
            TestSupport.find(root(activity), Button::class.java) { it.text == "Remove" }!!.performClick()
            TestSupport.idle()
            (ShadowDialog.getLatestDialog() as AlertDialog).getButton(AlertDialog.BUTTON_POSITIVE).performClick()
            TestSupport.await("removed") { text(activity).contains("ULJM99999 · Not installed") }
            assertFalse(file.exists())
        } finally {
            ScriptStore.indexUrl = ScriptStore.DEFAULT_INDEX_URL
            ScriptStore.fetcher = { throw ScriptStore.ScriptException("no network in tests") }
            File(context.getExternalFilesDir(null), "reader/profiles").deleteRecursively()
            ReaderCatalog.forget()
        }
    }

    @Test
    fun backlogListsLinesOldestFirstAndLooksWordsUp() {
        installDictionary()
        val db = ReaderSession.database(context)
        val lines = listOf(
            Triple("声", "…君は旅人だ？\n道を見張れ", listOf(RubySpan(1, 1, "きみ"), RubySpan(3, 2, "たびびと"), RubySpan(8, 1, "みち"), RubySpan(10, 2, "みは"))),
            Triple("少年", "私の名前は　まだない…", emptyList()),
            Triple("", "太郎は花子と共に\n港の朝市にやってきた", emptyList())
        )
        lines.forEachIndexed { index, (speaker, body, ruby) ->
            db.insertLine(
                game,
                TextLine(0, 0, -1, "dialogue", speaker, body, ruby, "s$index", emptyMap(), 1_759_000_000_000 + index * 60_000),
                LineAccounting(ReadingRules.countCharacters(body), index == 1, true, null)
            )
        }

        val intent = Intent(context, ReaderBacklogActivity::class.java).putExtra(ReaderBacklogActivity.EXTRA_GAME, game)
        val activity = Robolectric.buildActivity(ReaderBacklogActivity::class.java, intent).setup().get()
        val list = TestSupport.find(root(activity), RecyclerView::class.java)!!
        TestSupport.await("backlog rows") { list.adapter!!.itemCount == 3 }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-backlog")
        val shown = text(activity)
        assertTrue(shown, shown.indexOf("道を見張れ") < shown.indexOf("私の名前は"))
        assertTrue(shown, shown.indexOf("私の名前は") < shown.indexOf("港の朝市"))
        assertTrue(shown, shown.contains("声 · "))

        // A tap on 見張れ in the first row opens the popup with the dictionary entry.
        val first = TestSupport.find(list, ReaderTextView::class.java) { it.text.toString().contains("見張れ") }!!
        first.onCharacterTap!!.invoke(10, android.graphics.Rect())
        TestSupport.await("popup") {
            TestSupport.find(root(activity), LookupPopupView::class.java) { it.visibility == View.VISIBLE } != null
        }
        TestSupport.snapshot(root(activity), 1920, 1080, "screen-backlog-lookup")
        val popup = TestSupport.find(root(activity), LookupPopupView::class.java)!!
        assertTrue(TestSupport.allText(popup).contains("to keep watch"))

        // Searching narrows the list; clearing the search brings everything back.
        val search = TestSupport.find(root(activity), EditText::class.java)!!
        search.setText("朝市")
        TestSupport.await("search result") { list.adapter!!.itemCount == 1 }
        search.setText("")
        TestSupport.await("full list again") { list.adapter!!.itemCount == 3 }
    }

    @Test
    fun backlogOfAGameWithNoLinesSaysSo() {
        val intent = Intent(context, ReaderBacklogActivity::class.java).putExtra(ReaderBacklogActivity.EXTRA_GAME, game)
        val activity = Robolectric.buildActivity(ReaderBacklogActivity::class.java, intent).setup().get()
        TestSupport.await("empty backlog message") {
            TestSupport.find(root(activity), TextView::class.java) {
                it.text.toString() == "Nothing read yet in this game." && it.visibility == View.VISIBLE
            } != null
        }
    }
}
