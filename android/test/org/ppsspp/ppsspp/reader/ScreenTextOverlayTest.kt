// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.app.Activity
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.os.Looper
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.TextView
import androidx.preference.PreferenceManager
import java.io.File
import java.time.Duration
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryStore
import org.ppsspp.ppsspp.reader.mining.CardMedia
import org.ppsspp.ppsspp.reader.mining.Miner
import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.screen.Area
import org.ppsspp.ppsspp.reader.screen.ScreenGeometry
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.ui.AudioPromptView
import org.ppsspp.ppsspp.reader.ui.LookupPopupView
import org.ppsspp.ppsspp.reader.ui.ReaderHost
import org.ppsspp.ppsspp.reader.ui.ReaderOverlay
import org.ppsspp.ppsspp.reader.ui.ReaderTextView
import org.ppsspp.ppsspp.reader.ui.ScreenTextLayer
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
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog
import org.robolectric.shadows.ShadowToast

/**
 * The game's own text as something to tap: the real overlay views over a stand-in for the game
 * picture, driven by real touch events. A tap on a character of the game's text box looks up
 * the word that starts there; nothing that begins inside the box reaches the game; everything
 * else does.
 *
 * The stand-in draws made-up text at the positions a game would report, so the pictures
 * kept with READER_PREVIEW_DIR show whether the mark lands on the word.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = "w834dp-h469dp-369dpi") // an AYN Thor's upper display: 1920x1080
class ScreenTextOverlayTest {
    private lateinit var activity: Activity
    private lateinit var root: FrameLayout
    private lateinit var overlay: ReaderOverlay
    private lateinit var layer: ScreenTextLayer
    private var gameTaps = 0

    private val host = object : ReaderOverlay.Host {
        override fun openBacklog() {}
        override fun openStatistics() {}
        override fun openSettings() {}
        override fun openDictionaries() {}
        override fun openAnkiSetup() {}
        override fun chooseCardAudio() {}
        override fun hasSecondaryScreen() = false
    }

    // The picture large on the left, a trackpad area small beside it.
    private val topArea = Area(0f, 115f, 1416f, 965f)
    private val bottomArea = Area(1416f, 115f, 1920f, 493f)
    private val geometry = ScreenGeometry(1920, 1080, listOf(topArea), listOf(bottomArea))

    private val text = "今日は良い天気だ\n散歩に行こう"
    private val line = TextLine(
        sequence = 7, frame = 1, audioPosition = 0, channel = "dialogue", speaker = "",
        text = text, ruby = emptyList(), sourceId = "made-up", meta = emptyMap(), timestamp = 1,
        window = "w"
    )
    private val presented = PresentedLine(line, LineAccounting(14, false, true, 1), 7)

    /** Two rows from x 40, 16 pixels a character, at y 191 and 217: a message window. */
    private val layout = ScreenLayout.fromJson(
        buildString {
            append("""{"window":"w","seq":7,"space":[400,240],"box":[0,179,400,61],"glyphs":[""")
            var x = 40
            var y = 191
            text.forEachIndexed { i, c ->
                if (i > 0) append(',')
                if (c == '\n') {
                    append("null")
                    x = 40
                    y += 26
                } else {
                    append("[$x,$y,16,21]")
                    x += 16
                }
            }
            append("]}")
        }
    )!!

    private fun scaleX(x: Float) = topArea.left + x * topArea.width / 400f
    private fun scaleY(y: Float) = topArea.top + y * topArea.height / 240f

    /** Centre of the character at [offset] of the text, where the game draws it. */
    private fun centreOf(offset: Int): Pair<Float, Float> {
        val cell = layout.cells[offset]!!
        return scaleX(cell.x + cell.width / 2) to scaleY(cell.y + cell.height / 2)
    }

    /** Draws what the game would: a text box with the text at its reported positions. */
    private inner class GamePicture(context: Context) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        var showText = true

        override fun onDraw(canvas: Canvas) {
            canvas.drawColor(0xFF2B3A4A.toInt())
            paint.color = 0xFF10151B.toInt()
            canvas.drawRect(bottomArea.left, bottomArea.top, bottomArea.right, bottomArea.bottom, paint)
            if (!showText) return
            paint.color = 0xFFF2F2EE.toInt()
            canvas.drawRect(scaleX(8f), scaleY(180f), scaleX(392f), scaleY(240f), paint)
            paint.color = 0xFF3A0A0A.toInt()
            paint.textSize = topArea.height / 240f * 15f
            TestSupport.previewFont()?.let { paint.typeface = it }
            text.forEachIndexed { i, c ->
                val cell = layout.cells[i] ?: return@forEachIndexed
                canvas.drawText(c.toString(), scaleX(cell.x + 0.5f), scaleY(cell.y + 16f), paint)
            }
        }
    }

    private lateinit var game: GamePicture

    @Before
    fun setUp() {
        // Every test has its own data directory; a dictionary store left by another test class
        // would point at files that are gone, or at that class's dictionary.
        TestSupport.resetStores()
        activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(com.google.android.material.R.style.Theme_Material3_DayNight_NoActionBar)
        root = FrameLayout(activity)
        game = GamePicture(activity).apply { setOnClickListener { gameTaps++ } }
        root.addView(game, FrameLayout.LayoutParams(-1, -1))
        overlay = ReaderOverlay(activity, host, secondary = false).also { it.gameSurface = game }
        root.addView(overlay)
        activity.setContentView(root, ViewGroup.LayoutParams(1920, 1080))
        layer = TestSupport.find(root, ScreenTextLayer::class.java)!!
        layer.geometrySource = { geometry }
        layout()
    }

    @After
    fun tearDown() {
        anki?.close()
        Miner.forget()
        Miner.mediaOverride = null
        runningGame("")
        ReaderSession.screen.clear()
        TestSupport.resetStores()
    }

    private fun layout() {
        repeat(2) {
            TestSupport.idle()
            TestSupport.applyFont(root)
            root.measure(
                View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY)
            )
            root.layout(0, 0, 1920, 1080)
        }
    }

    /** The game shows the line: its text arrives, then where it is. */
    private fun showInGame() {
        overlay.onLine(presented)
        ReaderSession.screen.adoptLine(presented)
        ReaderSession.screen.applyLayout(layout)
        overlay.onScreenTextChanged()
        layout()
    }

    private fun tap(x: Float, y: Float) {
        val now = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x, y, 0)
        val up = MotionEvent.obtain(now, now + 40, MotionEvent.ACTION_UP, x, y, 0)
        root.dispatchTouchEvent(down)
        root.dispatchTouchEvent(up)
        down.recycle()
        up.recycle()
        TestSupport.idle()
    }

    /** [more] is further rows for the term bank, each starting with a comma. */
    private fun installDictionary(more: String = "") {
        val store = DictionaryStore.get(activity)
        val zip = File(activity.cacheDir, "screen-dict.zip")
        ZipOutputStream(zip.outputStream()).use { out ->
            out.putNextEntry(ZipEntry("index.json"))
            out.write("""{"title":"Test Dict","revision":"1","format":3}""".toByteArray())
            out.closeEntry()
            out.putNextEntry(ZipEntry("term_bank_1.json"))
            out.write(
                """[["天気","てんき","n","",10,["weather"],1,""],
                    ["散歩","さんぽ","n vs","vs",9,["walk","stroll"],2,""],
                    ["行く","いく","v5","v5",8,["to go"],3,""],
                    ["今日","きょう","n","",7,["today"],4,""],
                    ["良い","よい","adj-i","adj-i",6,["good"],5,""]$more]""".toByteArray()
            )
            out.closeEntry()
        }
        DictionaryImporter(store.database).import(zip)
    }

    private fun popup(): LookupPopupView = TestSupport.find(root, LookupPopupView::class.java)!!

    private fun awaitPopup(containing: String): LookupPopupView {
        TestSupport.await("the popup to show $containing") {
            popup().visibility == View.VISIBLE && TestSupport.allText(popup()).contains(containing)
        }
        layout()
        return popup()
    }

    private fun panel(): View = TestSupport.find(root, ReaderTextView::class.java)!!.let {
        it.parent.parent.parent as View
    }

    @Test
    fun tappingAWordInTheGamesOwnTextLooksItUp() {
        installDictionary()
        showInGame()

        // Nothing of the reader is on screen: no panel, no marks.
        assertEquals(View.GONE, panel().visibility)
        assertEquals(View.GONE, popup().visibility)
        assertTrue(layer.marks.isEmpty())
        TestSupport.snapshot(root, 1920, 1080, "screen-text-idle")

        // 天気 starts at offset 5.
        val (x, y) = centreOf(5)
        tap(x, y)
        val popup = awaitPopup("天気")
        TestSupport.snapshot(root, 1920, 1080, "screen-text-lookup")
        val shown = TestSupport.allText(popup)
        assertTrue(shown, shown.contains("てんき"))
        assertTrue(shown, shown.contains("weather"))
        assertEquals("looking a word up must not touch the game", 0, gameTaps)

        // The word is marked where the game draws it: the cells of 天 and 気.
        val mark = layer.marks.single()
        assertEquals(scaleX(120f), mark.left, 0.5f)
        assertEquals(scaleX(152f), mark.right, 0.5f)
        assertEquals(scaleY(191f), mark.top, 0.5f)
        assertEquals(scaleY(212f), mark.bottom, 0.5f)

        // The popup is on screen and clear of the game's text box.
        val box = layer.texts.single().box
        assertTrue("popup on screen", popup.top >= 0 && popup.bottom <= 1080)
        assertTrue(
            "popup (${popup.top} to ${popup.bottom}) keeps off the text box (${box.top} to ${box.bottom})",
            popup.bottom <= box.top || popup.top >= box.bottom
        )
        assertEquals("the panel stays away", View.GONE, panel().visibility)
    }

    @Test
    fun aConjugatedWordIsFoundFromItsFirstCharacter() {
        installDictionary()
        showInGame()
        val (x, y) = centreOf(12) // 行 of 行こう
        tap(x, y)
        val shown = TestSupport.allText(awaitPopup("行く"))
        assertTrue(shown, shown.contains("to go"))
        assertEquals(3, layer.marks.single().let { ((it.right - it.left) / (scaleX(16f) - scaleX(0f)) + 0.5f).toInt() })
    }

    @Test
    fun theTextBoxIsSafeAndTheRestOfTheScreenIsTheGames() {
        installDictionary()
        showInGame()

        // In the text box, right of the first row: not a character, and not the game either.
        tap(scaleX(300f), scaleY(201f))
        assertEquals(0, gameTaps)
        assertEquals(View.GONE, popup().visibility)
        // Left of the last row, still in the box. (On a character a lookup would start, and
        // its popup, arriving whenever the lookup is done, would take the taps that follow.)
        tap(scaleX(10f), scaleY(228f))
        assertEquals(0, gameTaps)
        assertEquals(View.GONE, popup().visibility)

        // Above the box, and on the other screen: the game.
        tap(scaleX(200f), scaleY(100f))
        assertEquals(1, gameTaps)
        tap(bottomArea.left + 100f, bottomArea.top + 100f)
        assertEquals(2, gameTaps)
    }

    @Test
    fun anotherWordWithOneTapWhileThePopupIsOpenAndTheBoxClosesIt() {
        installDictionary()
        showInGame()
        val (x, y) = centreOf(5)
        tap(x, y)
        awaitPopup("天気")

        val (x2, y2) = centreOf(9) // 散歩
        tap(x2, y2)
        val shown = TestSupport.allText(awaitPopup("散歩"))
        assertTrue(shown, shown.contains("さんぽ"))
        assertEquals(scaleX(40f), layer.marks.single().left, 0.5f)
        assertEquals(scaleY(217f), layer.marks.single().top, 0.5f)

        // A tap in the box beside the text puts the popup away; nothing reaches the game.
        tap(scaleX(300f), scaleY(201f))
        assertEquals(View.GONE, popup().visibility)
        assertTrue(layer.marks.isEmpty())
        assertEquals(0, gameTaps)
    }

    @Test
    fun aTapElsewhereClosesThePopupWithoutReachingTheGame() {
        installDictionary()
        showInGame()
        val (x, y) = centreOf(0)
        tap(x, y)
        awaitPopup("今日")

        tap(scaleX(200f), scaleY(20f))
        assertEquals(View.GONE, popup().visibility)
        assertEquals("closing the popup must not click into the game", 0, gameTaps)
        tap(scaleX(200f), scaleY(20f))
        assertEquals(1, gameTaps)
    }

    @Test
    fun whenTheTextLeavesTheScreenItsPlaceBelongsToTheGameAgain() {
        showInGame()
        val (x, y) = centreOf(5)
        ReaderSession.screen.applyLayout(ScreenLayout.fromJson("""{"window":"w","seq":7,"closed":true}""")!!)
        overlay.onScreenTextChanged()
        game.showText = false
        tap(x, y)
        assertEquals(1, gameTaps)
        assertTrue(layer.texts.isEmpty())
        // The panel does not jump in for a line that could be read in the game's picture.
        assertEquals(View.GONE, panel().visibility)
    }

    @Test
    fun thePanelShowsALineTheGameDoesNotPlace() {
        // The line says which window it is in, but no position follows.
        overlay.onLine(presented)
        layout()
        assertEquals("give the position a moment to arrive", View.GONE, panel().visibility)
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(600))
        layout()
        assertEquals(View.VISIBLE, panel().visibility)
        TestSupport.snapshot(root, 1920, 1080, "screen-text-panel-fallback")

        // The position arrives after all: the panel makes way.
        ReaderSession.screen.adoptLine(presented)
        ReaderSession.screen.applyLayout(layout)
        overlay.onScreenTextChanged()
        layout()
        assertEquals(View.GONE, panel().visibility)
    }

    @Test
    fun settingsSwitchTappingOffAndKeepThePanelBesideIt() {
        val prefs = PreferenceManager.getDefaultSharedPreferences(activity)
        showInGame()
        val (x, y) = centreOf(5)

        // Panel and tappable text together; the panel at the top, clear of the game's text box.
        prefs.edit().putBoolean(ReaderSettings.KEY_PANEL_AUTO_HIDE, false)
            .putString(ReaderSettings.KEY_PANEL_POSITION, ReaderSettings.POSITION_TOP).commit()
        TestSupport.idle()
        layout()
        assertEquals(View.VISIBLE, panel().visibility)
        assertEquals(1, layer.texts.size)

        // Outlines of what can be tapped, for checking positions on a device.
        prefs.edit().putBoolean(ReaderSettings.KEY_SCREEN_TEXT_OUTLINE, true).commit()
        TestSupport.idle()
        layout()
        TestSupport.snapshot(root, 1920, 1080, "screen-text-outline")

        // Tapping the game's text switched off: its text box is the game's again.
        prefs.edit().putBoolean(ReaderSettings.KEY_SCREEN_TEXT, false).commit()
        TestSupport.idle()
        layout()
        assertTrue(layer.texts.isEmpty())
        tap(x, y)
        assertEquals(1, gameTaps)
        assertEquals(View.VISIBLE, panel().visibility)

        prefs.edit().clear().commit()
    }

    // ---- The card box of the popup ----

    private var anki: AnkiConnectStub? = null
    private val density get() = activity.resources.displayMetrics.density

    /** Cards go to a stand-in AnkiConnect, into a note type whose first field is the word. */
    private fun useAnki(allowDuplicates: Boolean = false, url: String? = null): AnkiConnectStub {
        val stub = AnkiConnectStub().also { anki = it }
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putString(ReaderSettings.KEY_ANKI_TRANSPORT, ReaderSettings.ANKI_ANKICONNECT)
            .putString(ReaderSettings.KEY_ANKICONNECT_URL, url ?: stub.url)
            .putString(ReaderSettings.KEY_ANKI_DECK, "日本語::Mining")
            .putString(ReaderSettings.KEY_ANKI_MODEL, "Lapis")
            .putString(ReaderSettings.KEY_ANKI_FIELDS, """{"Expression":"{expression}","Sentence":"{sentence}"}""")
            .putBoolean(ReaderSettings.KEY_ANKI_DUPLICATES, allowDuplicates)
            .commit()
        TestSupport.idle()
        return stub
    }

    private fun box(description: String): ImageButton? =
        TestSupport.find(popup(), ImageButton::class.java) { it.contentDescription == description && it.isShown }

    private fun addBox() = box("Add to Anki")
    private fun bookBox() = box("Already in Anki: show the note")

    /** Where [view] is drawn in the root, as left, top, right, bottom. */
    private fun placeOf(view: View): IntArray {
        var x = 0
        var y = 0
        var current: View? = view
        while (current != null && current !== root) {
            x += current.left - current.scrollX + current.translationX.toInt()
            y += current.top - current.scrollY + current.translationY.toInt()
            current = current.parent as? View
        }
        return intArrayOf(x, y, x + view.width, y + view.height)
    }

    private fun queries(stub: AnkiConnectStub) =
        stub.received("findNotes").map { it.getJSONObject("params").getString("query") }

    @Test
    fun thePlusSitsInABoxAtTheBottomRightOfThePopupAndMakesACard() {
        val stub = useAnki()
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) } // 天気
        val popup = awaitPopup("weather")
        TestSupport.await("Anki to be asked about the word") { queries(stub).isNotEmpty() }
        assertEquals(listOf("\"deck:日本語::Mining\" \"Expression:天気\""), queries(stub))
        layout()
        TestSupport.snapshot(root, 1920, 1080, "popup-box-add")

        val add = addBox()!!
        assertNull("Anki does not have the word: no book", bookBox())
        assertNotNull("the plus is in a box", add.background)
        val at = placeOf(add)
        val within = placeOf(popup)
        assertTrue("box at the popup's right edge", within[2] - at[2] in 0..(8 * density).toInt())
        assertTrue("box at the popup's bottom edge", within[3] - at[3] in 0..(8 * density).toInt())

        // The text stops short of the box: its last line ends in a blank as wide as the box.
        val senses = TestSupport.find(popup, TextView::class.java) { it.text.toString().contains("weather") }!!
        val lastLine = senses.layout.lineCount - 1
        val textRight = placeOf(senses)[0] + senses.layout.getLineRight(lastLine)
        assertTrue("the last line ($textRight) ends before the box (${at[2]})", textRight <= at[2] + 1)
        val written = senses.paint.measureText("1 n weather")
        assertTrue(
            "the word of the last line is left of the box",
            placeOf(senses)[0] + written <= at[2] - 24 * density + 1
        )

        add.performClick()
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        val note = stub.received("addNote").single().getJSONObject("params").getJSONObject("note")
        assertEquals("日本語::Mining", note.getString("deckName"))
        assertEquals("天気", note.getJSONObject("fields").getString("Expression"))
        assertEquals("今日は良い<b>天気</b>だ<br>散歩に行こう", note.getJSONObject("fields").getString("Sentence"))
        assertEquals("Anki was asked once more before the note was sent", 2, queries(stub).size)
        assertEquals("the note type's first field was asked for once", 1, stub.received("modelFieldNames").size)

        // The word is in Anki now: the box shows the open book, and the book shows the note.
        TestSupport.await("the book") { bookBox() != null }
        assertNull(addBox())
        assertEquals("Card added", ShadowToast.getTextOfLatestToast())
        bookBox()!!.performClick()
        TestSupport.await("Anki's browser to be asked for") { stub.received("guiBrowse").isNotEmpty() }
        assertEquals(
            "nid:1496198395707",
            stub.received("guiBrowse").single().getJSONObject("params").getString("query")
        )
        assertEquals("none of this touched the game", 0, gameTaps)
    }

    @Test
    fun aWordAnkiAlreadyHasShowsAnOpenBookInstead() {
        val stub = useAnki()
        stub.find = { query -> if (query.contains("天気")) listOf(11L, 12L) else emptyList() }
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) }
        val popup = awaitPopup("weather")
        TestSupport.await("the book") { bookBox() != null }
        layout()
        TestSupport.snapshot(root, 1920, 1080, "popup-box-in-deck")
        assertNull("no plus for a word that is there", addBox())
        val at = placeOf(bookBox()!!)
        val within = placeOf(popup)
        assertTrue(within[2] - at[2] in 0..(8 * density).toInt())
        assertTrue(within[3] - at[3] in 0..(8 * density).toInt())

        bookBox()!!.performClick()
        TestSupport.await("Anki's browser to be asked for") { stub.received("guiBrowse").isNotEmpty() }
        assertEquals("nid:11,12", stub.received("guiBrowse").single().getJSONObject("params").getString("query"))
        assertTrue("nothing was added", stub.received("addNote").isEmpty())

        // Another word of the same line, which Anki does not have: the plus again.
        centreOf(9).let { (x, y) -> tap(x, y) } // 散歩
        awaitPopup("stroll")
        TestSupport.await("Anki to be asked about 散歩") { queries(stub).any { it.contains("散歩") } }
        TestSupport.idle()
        assertNotNull(addBox())
        assertNull(bookBox())
    }

    @Test
    fun withDuplicatesAllowedThePlusStaysBesideTheBook() {
        val stub = useAnki(allowDuplicates = true)
        stub.find = { listOf(11L) }
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) }
        val popup = awaitPopup("weather")
        TestSupport.await("the book") { bookBox() != null }
        layout()
        TestSupport.snapshot(root, 1920, 1080, "popup-box-both")
        val book = placeOf(bookBox()!!)
        val add = placeOf(addBox()!!)
        val within = placeOf(popup)
        assertTrue("the book is left of the plus", book[2] <= add[0] + 1)
        assertTrue("both inside the popup", book[0] >= within[0] && add[2] <= within[2])
        val senses = TestSupport.find(popup, TextView::class.java) { it.text.toString().contains("weather") }!!
        assertTrue(
            "the text is left of both boxes",
            placeOf(senses)[0] + senses.paint.measureText("1 n weather") <= book[2] - 24 * density + 1
        )

        addBox()!!.performClick()
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        val options = stub.received("addNote").single().getJSONObject("params").getJSONObject("note")
            .getJSONObject("options")
        assertTrue(options.getBoolean("allowDuplicate"))
        TestSupport.await("the book again") { bookBox() != null && addBox()?.isEnabled == true }
        bookBox()!!.performClick()
        TestSupport.await("Anki's browser to be asked for") { stub.received("guiBrowse").isNotEmpty() }
        assertEquals(
            "the old note and the new one",
            "nid:11,1496198395707",
            stub.received("guiBrowse").single().getJSONObject("params").getString("query")
        )
    }

    @Test
    fun aWordAddedElsewhereMeanwhileIsNotAddedAgain() {
        val stub = useAnki()
        var asked = 0
        stub.find = { if (asked++ == 0) emptyList() else listOf(5L) }
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) }
        awaitPopup("weather")
        TestSupport.await("Anki to be asked about the word") { queries(stub).isNotEmpty() }
        TestSupport.idle()
        addBox()!!.performClick()
        TestSupport.await("the book") { bookBox() != null }
        assertEquals("Already in Anki", ShadowToast.getTextOfLatestToast())
        assertTrue("AnkiConnect Android would have added it: it must not be asked to", stub.received("addNote").isEmpty())
        assertNull(addBox())
    }

    @Test
    fun anAnkiThatDoesNotAnswerLeavesThePlus() {
        val gone = AnkiConnectStub()
        val address = gone.url
        gone.close()
        useAnki(url = address)
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) }
        awaitPopup("weather")
        Thread.sleep(300)
        TestSupport.idle()
        assertNotNull(addBox())
        assertNull(bookBox())
        addBox()!!.performClick()
        TestSupport.await("the failure to be reported") {
            ShadowToast.getTextOfLatestToast()?.startsWith("Card not added") == true
        }
        assertNotNull("the plus is there to try again", addBox())
        assertTrue(addBox()!!.isEnabled)
    }

    @Test
    fun thePopupStaysShortAndTheBoxOfALongEntryWaitsInItsCorner() {
        // 良い with a dozen senses more: far more than a short popup shows at once.
        installDictionary(
            (2..13).joinToString("") { n -> """,["良い","よい","adj-i","adj-i",6,["sense number $n"],5,""]""" }
        )
        showInGame()
        centreOf(3).let { (x, y) -> tap(x, y) } // 良い
        val popup = awaitPopup("sense number 13")
        layout()
        TestSupport.snapshot(root, 1920, 1080, "popup-short")
        val within = placeOf(popup)
        assertTrue("a quarter of the screen at most: ${popup.height}", popup.height <= 1080 * 27 / 100)
        assertTrue("and it uses what it may", popup.height > 1080 * 20 / 100)

        // The entry ends far below, yet its box is in the popup's bottom right corner.
        val add = addBox()!!
        val waiting = placeOf(add)
        assertTrue("box at the popup's right edge", within[2] - waiting[2] in 0..(8 * density).toInt())
        assertTrue(
            "box at the popup's bottom edge (${waiting[3]} of ${within[3]})",
            within[3] - waiting[3] in 0..(8 * density).toInt()
        )
        add.performClick() // and it can be pressed there (no Anki is set up: it says so)
        TestSupport.await("the answer") { ShadowToast.getTextOfLatestToast() != null }

        // Scrolled to the end, the box is where the entry ends: beside its last line.
        popup.scrollTo(100_000)
        layout()
        assertTrue(popup.scrolled > 0)
        val settled = placeOf(addBox()!!)
        val last = TestSupport.find(popup, TextView::class.java) { it.text.toString().contains("sense number 13") }!!
        assertTrue("box beside the last line", settled[3] >= placeOf(last)[3] && settled[3] <= within[3])

        // How tall the popup may get is a setting.
        tap(scaleX(200f), scaleY(100f))
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putInt(ReaderSettings.KEY_POPUP_HEIGHT, 60).commit()
        TestSupport.idle()
        centreOf(3).let { (x, y) -> tap(x, y) }
        val taller = awaitPopup("sense number 13")
        layout()
        assertTrue("taller now: ${taller.height}", taller.height > 1080 * 27 / 100)
        assertTrue(taller.height <= 1080 * 60 / 100)
        PreferenceManager.getDefaultSharedPreferences(activity).edit().clear().commit()
    }

    // ---- The session line ----

    private fun sessionLine(): TextView? =
        TestSupport.find(root, TextView::class.java) { it.text.contains("字") && it.isShown }

    @Test
    fun theSessionSoFarIsShownSmallInTheTopRightCorner() {
        showInGame()
        assertNull("nothing was counted yet", sessionLine())
        overlay.onSessionActivity(ReadingCounters(activeMs = 12 * 60_000L, characters = 1234, lines = 80))
        layout()
        TestSupport.snapshot(root, 1920, 1080, "session-line")
        val line = sessionLine()!!
        assertEquals("1,234字 · 6,170/h · 12m", line.text.toString())
        val at = placeOf(line)
        assertTrue("at the right edge", 1920 - at[2] in 0..(12 * density).toInt())
        assertTrue("at the top", at[1] in 0..(8 * density).toInt())
        assertTrue("small: ${line.height} high", line.height <= 20 * density)
        assertTrue("dim: the game shows through", (line.currentTextColor ushr 24) < 0xE0)

        // It is not in the way: a tap on it is the game's.
        tap((at[0] + at[2]) / 2f, (at[1] + at[3]) / 2f)
        assertEquals(1, gameTaps)

        // It follows the session, says when counting is paused, and can be switched off.
        overlay.onSessionActivity(ReadingCounters(activeMs = 13 * 60_000L, characters = 1300, lines = 84))
        assertEquals("1,300字 · 6,000/h · 13m", sessionLine()!!.text.toString())
        val prefs = PreferenceManager.getDefaultSharedPreferences(activity)
        prefs.edit().putBoolean(ReaderSettings.KEY_STATS_PAUSED, true).commit()
        TestSupport.idle()
        // Paused: a small mark before the line, and the line as it was.
        assertEquals("1,300字 · 6,000/h · 13m", sessionLine()!!.text.toString())
        assertTrue(overlay.pauseShown)
        assertNotNull("the mark is drawn before the line", sessionLine()!!.compoundDrawables[0])
        layout()
        TestSupport.snapshot(root, 1920, 1080, "session-line-paused")
        prefs.edit().putBoolean(ReaderSettings.KEY_SESSION_HUD, false).commit()
        TestSupport.idle()
        // Without the line the mark stays, alone in the corner: paused must not go unnoticed.
        assertNull(sessionLine())
        assertTrue(overlay.pauseShown)
        val mark = TestSupport.find(root, TextView::class.java) {
            it.isShown && it.contentDescription == "Reading statistics are paused"
        }
        assertNotNull(mark)
        prefs.edit().putBoolean(ReaderSettings.KEY_STATS_PAUSED, false).commit()
        TestSupport.idle()
        assertFalse(overlay.pauseShown)
        assertNull(TestSupport.find(root, TextView::class.java) {
            it.isShown && it.contentDescription == "Reading statistics are paused"
        })
        prefs.edit().clear().commit()
        TestSupport.idle()
        assertNotNull("on unless switched off", sessionLine())
        assertNull("not paused: no mark", sessionLine()!!.compoundDrawables[0])
        overlay.onSessionActivity(null)
        assertNull("the game was closed", sessionLine())
    }

    @Test
    fun theSecondScreenHasNoSessionLine() {
        val second = ReaderOverlay(activity, host, secondary = true)
        root.addView(second)
        layout()
        second.onSessionActivity(ReadingCounters(activeMs = 60_000L, characters = 50))
        layout()
        assertFalse(TestSupport.allText(second).contains("字"))
    }

    @Test
    fun theLayerFollowsTheScreensWhenTheyAreRearranged() {
        showInGame()
        assertNotNull(layer.texts.singleOrNull())
        // The picture moves to the right half of the display.
        val moved = Area(960f, 0f, 1920f, 576f)
        layer.geometrySource = { ScreenGeometry(1920, 1080, listOf(moved), emptyList()) }
        // No notice is given; the next touch looks again.
        tap(scaleX(48f), scaleY(201f)) // where the text was
        assertEquals(1, gameTaps)
        // Where its box is now, beside the text: on a character a lookup would start, and its
        // popup would take the tap further down.
        tap(moved.left + 300f * 2.4f, moved.top + 201f * 2.4f)
        assertEquals(1, gameTaps)
        assertEquals(moved.left + 40f * 2.4f, layer.texts.single().characters.first().drawn.left, 0.5f)

        // No surface to map onto (the game is starting up): nothing is taken.
        layer.geometrySource = { null }
        tap(moved.left + 300f * 2.4f, moved.top + 201f * 2.4f)
        assertEquals(2, gameTaps)
    }

    // ---- The line's audio on a card ----

    /** Stands in for the game: every line has a clip of what the game played, a small file. */
    private class Clips(private val directory: File) : CardMedia {
        val made = ArrayList<File>()

        override fun captureNow(callback: (File?) -> Unit) = callback(null)

        override fun screenshotFor(backlogId: Long): File? = null

        override fun audioClip(backlogId: Long, start: Long, end: Long?, callback: (File?) -> Unit) {
            val file = File(directory, "clip-${made.size}.m4a").apply { writeBytes(ByteArray(64) { 1 }) }
            made.add(file)
            callback(file)
        }
    }

    /**
     * Cards go to the stand-in Anki, into a note type with a field for the line's audio, and
     * the game has a clip for every line. [general] is the choice in Reader settings; left out,
     * nothing was chosen there.
     */
    private fun useAudio(general: String? = null): Pair<AnkiConnectStub, Clips> {
        val stub = useAnki()
        stub.models = mapOf("Lapis" to listOf("Expression", "Sentence", "SentenceAudio"))
        val editor = PreferenceManager.getDefaultSharedPreferences(activity).edit().putString(
            ReaderSettings.KEY_ANKI_FIELDS,
            """{"Expression":"{expression}","Sentence":"{sentence}","SentenceAudio":"{audio}"}"""
        )
        if (general != null) editor.putString(ReaderSettings.KEY_CARD_AUDIO, general)
        editor.commit()
        val clips = Clips(activity.cacheDir)
        Miner.mediaOverride = clips
        return stub to clips
    }

    /** Says which game is running, as the session would: [key] is its title ID, or empty. */
    private fun runningGame(key: String) {
        ReaderSession::class.java.getDeclaredField("titleId").apply {
            isAccessible = true
            set(null, key)
        }
    }

    private fun question(): AudioPromptView = TestSupport.find(root, AudioPromptView::class.java)!!

    private fun awaitQuestion(): AudioPromptView {
        TestSupport.await("the question about the audio") { question().isShown }
        layout()
        return question()
    }

    /** A real tap on the middle of the question's button that says [label]. */
    private fun tapAnswer(label: String) {
        val button = TestSupport.find(question(), TextView::class.java) { it.text.toString() == label }!!
        val at = placeOf(button)
        tap((at[0] + at[2]) / 2f, (at[1] + at[3]) / 2f)
    }

    /** The question takes no answer in its first moment: let that pass. */
    private fun letTheQuestionSettle() =
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(AudioPromptView.GUARD_MS + 50))

    private fun sentNote(stub: AnkiConnectStub) =
        stub.received("addNote").single().getJSONObject("params").getJSONObject("note").getJSONObject("fields")

    /** Looks 天気 up in the game's text and presses the plus of its entry. */
    private fun askForACard(stub: AnkiConnectStub) {
        installDictionary()
        showInGame()
        centreOf(5).let { (x, y) -> tap(x, y) }
        awaitPopup("weather")
        TestSupport.await("Anki to be asked about the word") { queries(stub).isNotEmpty() }
        layout()
        addBox()!!.performClick()
    }

    @Test
    fun aGameSetToNeverMakesCardsWithoutAudioAndDoesNotAsk() {
        val (stub, clips) = useAudio(general = "never")
        askForACard(stub)
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        assertEquals("", sentNote(stub).getString("SentenceAudio"))
        assertEquals("天気", sentNote(stub).getString("Expression"))
        assertTrue("no clip was even made", clips.made.isEmpty())
        assertTrue("nothing was stored in Anki's media", stub.received("storeMediaFile").isEmpty())
        assertFalse("nobody was asked", question().isShown)
    }

    @Test
    fun aGameSetToAlwaysMakesCardsWithTheirAudioAndDoesNotAsk() {
        val (stub, clips) = useAudio(general = "always")
        askForACard(stub)
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        val stored = stub.received("storeMediaFile").single().getJSONObject("params").getString("filename")
        assertTrue(stored, stored.endsWith(".m4a"))
        assertEquals("[sound:$stored]", sentNote(stub).getString("SentenceAudio"))
        assertFalse("nobody was asked", question().isShown)
        TestSupport.await("the clip to be removed") { !clips.made.single().exists() }
    }

    @Test
    fun unlessToldOtherwiseACardAsksAboutItsAudioAndWithoutLeavesItOut() {
        val (stub, clips) = useAudio()
        askForACard(stub)
        val question = awaitQuestion()
        TestSupport.snapshot(root, 1920, 1080, "card-audio-question")
        val said = TestSupport.allText(question)
        assertTrue(said, said.contains("Add the line's audio to the card?"))
        for (label in listOf("Listen", "With audio", "Without")) assertTrue(said, said.contains(label))

        // It is inside the picture, beside the popup and not over the text the word is in.
        val at = placeOf(question)
        val popupAt = placeOf(popup())
        assertTrue("within the screen: ${at.toList()}", at[0] >= 0 && at[1] >= 0 && at[2] <= 1920 && at[3] <= 1080)
        assertTrue("above the popup, which is above the text", at[3] <= popupAt[1])
        assertEquals("its right edge at the popup's", popupAt[2], at[2])

        // Nothing is sent while the question is open, and the plus cannot be pressed again.
        Thread.sleep(200)
        TestSupport.idle()
        assertTrue("no note before the answer", stub.received("addNote").isEmpty())
        assertFalse(addBox()!!.isEnabled)

        // The press that asked for the card is not an answer, however quickly it comes again.
        tapAnswer("With audio")
        assertTrue("an answer in the first moment is not taken", question.isShown)

        letTheQuestionSettle()
        tapAnswer("Without")
        assertFalse("answered: the question is gone", question.isShown)
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        assertEquals("", sentNote(stub).getString("SentenceAudio"))
        assertEquals("今日は良い<b>天気</b>だ<br>散歩に行こう", sentNote(stub).getString("Sentence"))
        assertTrue("nothing was stored in Anki's media", stub.received("storeMediaFile").isEmpty())
        assertFalse("the clip is removed", clips.made.single().exists())
        TestSupport.await("the book") { bookBox() != null }
        assertEquals("Card added", ShadowToast.getTextOfLatestToast())
        assertEquals("none of this touched the game", 0, gameTaps)
    }

    @Test
    fun answeredWithAudioTheCardGetsTheClip() {
        val (stub, clips) = useAudio(general = "ask")
        askForACard(stub)
        awaitQuestion()
        letTheQuestionSettle()
        tapAnswer("With audio")
        assertFalse(question().isShown)
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        val stored = stub.received("storeMediaFile").single().getJSONObject("params").getString("filename")
        assertEquals("[sound:$stored]", sentNote(stub).getString("SentenceAudio"))
        TestSupport.await("the clip to be removed") { !clips.made.single().exists() }
    }

    @Test
    fun aTapBesideTheQuestionCallsTheCardOff() {
        val (stub, clips) = useAudio()
        askForACard(stub)
        awaitQuestion()
        letTheQuestionSettle()
        ShadowToast.reset()

        tap(1700f, 60f) // on the game's picture, far from the popup and the question
        assertFalse("the question is gone", question().isShown)
        Thread.sleep(300)
        TestSupport.idle()
        assertTrue("no card", stub.received("addNote").isEmpty())
        assertTrue("nothing was stored in Anki's media", stub.received("storeMediaFile").isEmpty())
        assertFalse("the clip is removed", clips.made.single().exists())
        assertNull("nothing is said about a card the user called off", ShadowToast.getTextOfLatestToast())
        assertEquals("the tap went to nothing else: the game", 0, gameTaps)
        assertEquals("nor the popup, which is still open", View.VISIBLE, popup().visibility)
        assertTrue("and its plus can be pressed again", addBox()!!.isEnabled)

        // Pressed again, the question comes again.
        addBox()!!.performClick()
        awaitQuestion()
        assertEquals(2, clips.made.size)
    }

    @Test
    fun theClipCanBeHeardBeforeAnswering() {
        val (stub, clips) = useAudio()
        askForACard(stub)
        val question = awaitQuestion()
        letTheQuestionSettle()
        // Robolectric's player plays what it has been told about: one second, ready at once.
        org.robolectric.shadows.ShadowMediaPlayer.addMediaInfo(
            org.robolectric.shadows.util.DataSource.toDataSource(clips.made.single().path),
            org.robolectric.shadows.ShadowMediaPlayer.MediaInfo(1000, 0)
        )
        fun placeOfAnswer(label: String) =
            placeOf(TestSupport.find(question, TextView::class.java) { it.text.toString() == label }!!).toList()
        val before = placeOfAnswer("With audio") to placeOfAnswer("Without")
        tapAnswer("Listen")
        TestSupport.await("the button to offer stopping") { TestSupport.allText(question).contains("Stop") }
        layout()
        assertEquals(
            "the other buttons stay where they are while the first says Stop",
            before,
            placeOfAnswer("With audio") to placeOfAnswer("Without")
        )
        assertTrue("listening is not an answer", question.isShown)
        assertTrue(stub.received("addNote").isEmpty())

        // Played to its end, the button offers it again.
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(1200))
        assertTrue(TestSupport.allText(question), TestSupport.allText(question).contains("Listen"))

        // Stopped half way by the same button, and still nothing is answered.
        tapAnswer("Listen")
        TestSupport.await("the button to offer stopping") { TestSupport.allText(question).contains("Stop") }
        tapAnswer("Stop")
        assertTrue(TestSupport.allText(question).contains("Listen"))
        assertTrue(question.isShown)

        tapAnswer("With audio")
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        assertTrue(sentNote(stub).getString("SentenceAudio").startsWith("[sound:"))
    }

    @Test
    fun aSentenceCardAsksInTheMiddleOfTheScreen() {
        val (stub, _) = useAudio()
        showInGame()
        overlay.onCommand(ReaderSession.Command.SENTENCE_CARD)
        val question = awaitQuestion()
        TestSupport.snapshot(root, 1920, 1080, "card-audio-question-sentence")
        val at = placeOf(question)
        assertTrue("centred across: ${at.toList()}", at[0] + at[2] in 1919..1921)
        assertTrue("centred down: ${at.toList()}", at[1] + at[3] in 1079..1081)

        letTheQuestionSettle()
        tapAnswer("With audio")
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        assertEquals("今日は良い天気だ<br>散歩に行こう", sentNote(stub).getString("Expression"))
        assertTrue(sentNote(stub).getString("SentenceAudio").startsWith("[sound:"))
    }

    @Test
    fun aGamesOwnChoiceComesBeforeTheGeneralOne() {
        val settings = ReaderSettings(activity)
        val aceAttorney = "ULJS00219"
        val other = "ULJM05001"
        assertEquals("nothing chosen anywhere: ask", ReaderSettings.CardAudio.ASK, settings.cardAudioFor(aceAttorney))
        assertNull(settings.cardAudioOf(aceAttorney))

        settings.setCardAudioOf(aceAttorney, ReaderSettings.CardAudio.NEVER)
        TestSupport.idle()
        assertEquals(ReaderSettings.CardAudio.NEVER, settings.cardAudioFor(aceAttorney))
        assertEquals("another game is not affected", ReaderSettings.CardAudio.ASK, settings.cardAudioFor(other))

        // The running game's own choice is what its cards go by, whatever the general one says.
        val (stub, clips) = useAudio(general = "always")
        assertEquals(ReaderSettings.CardAudio.ALWAYS, settings.cardAudioFor(other))
        runningGame(aceAttorney)
        askForACard(stub)
        TestSupport.await("the note to be sent") { stub.received("addNote").isNotEmpty() }
        assertEquals("", sentNote(stub).getString("SentenceAudio"))
        assertTrue(clips.made.isEmpty())

        // Taken back, the game follows the general choice again.
        settings.setCardAudioOf(aceAttorney, null)
        TestSupport.idle()
        assertNull(settings.cardAudioOf(aceAttorney))
        assertEquals(ReaderSettings.CardAudio.ALWAYS, settings.cardAudioFor(aceAttorney))
    }

    @Test
    fun theReaderMenuOpensOverTheEmulatorsOwnScreen() {
        // The game's screen has the emulator's theme, which is not the reader's: the menu
        // and the list under it are made all the same (they once ended the app there).
        val plain = Robolectric.buildActivity(Activity::class.java).setup().get()
        plain.setTheme(android.R.style.Theme_DeviceDefault)
        runningGame("ULJM05000")
        val menu = ReaderHost(plain, { false })
        menu.showMenu(View(plain))
        TestSupport.idle()
        val shown = ShadowDialog.getLatestDialog() as androidx.appcompat.app.AlertDialog
        assertTrue(shown.isShowing)
        val entries = (0 until shown.listView.adapter.count).map { shown.listView.adapter.getItem(it).toString() }
        assertTrue("the menu has the reload entry: $entries", entries.contains("Reload text profile"))
        shown.dismiss()
        menu.chooseCardAudio()
        TestSupport.idle()
        assertTrue((ShadowDialog.getLatestDialog() as androidx.appcompat.app.AlertDialog).isShowing)
    }

    @Test
    fun theReaderMenuSetsWhatTheRunningGamesCardsDoWithTheirAudio() {
        val game = "ULJM05000"
        runningGame(game)
        val settings = ReaderSettings(activity)
        val menu = ReaderHost(activity, { false })

        fun open(): androidx.appcompat.app.AlertDialog {
            menu.chooseCardAudio()
            TestSupport.idle()
            return ShadowDialog.getLatestDialog() as androidx.appcompat.app.AlertDialog
        }
        var dialog = open()
        val offered = (0 until dialog.listView.adapter.count).map { dialog.listView.adapter.getItem(it).toString() }
        assertEquals(
            listOf("As in Reader settings (Ask for each card)", "Always add it", "Ask for each card", "Never add it"),
            offered
        )
        assertEquals("the game has no choice of its own yet", 0, dialog.listView.checkedItemPosition)

        dialog.listView.performItemClick(null, 3, dialog.listView.adapter.getItemId(3))
        TestSupport.idle()
        assertFalse("chosen: the list closes", dialog.isShowing)
        assertEquals(ReaderSettings.CardAudio.NEVER, settings.cardAudioOf(game))
        assertEquals("only this game was changed", ReaderSettings.CardAudio.ASK, settings.cardAudioGeneral)

        // Opened again it shows the choice, and the first entry takes it back.
        dialog = open()
        assertEquals(3, dialog.listView.checkedItemPosition)
        dialog.listView.performItemClick(null, 0, dialog.listView.adapter.getItemId(0))
        TestSupport.idle()
        assertNull(settings.cardAudioOf(game))

        // A game whose profile says its lines have no voices: that is what its first entry is.
        runningGame("ULJS00219")
        dialog = open()
        assertEquals("As set for this game (Never add it)", dialog.listView.adapter.getItem(0).toString())
        assertEquals(0, dialog.listView.checkedItemPosition)
    }
}
