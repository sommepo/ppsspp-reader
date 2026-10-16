// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.R
import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Typeface
import android.os.Looper
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryStore
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.ui.LookupPopupView
import org.ppsspp.ppsspp.reader.ui.ReaderOverlay
import org.ppsspp.ppsspp.reader.ui.ReaderTextView
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/**
 * Lays out and draws the real overlay views off-device and drives them with real touch events:
 * a tap on a character must look up the word that starts there, show the popup, mark the word
 * and leave the rest of the screen to the game.
 *
 * Set READER_PREVIEW_DIR to keep PNGs of what was drawn, and READER_PREVIEW_FONT to a font with
 * Japanese glyphs so they are readable. Neither is needed for the checks.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = "w768dp-h432dp-400dpi") // 1920x1080 at a handheld's density
class ReaderOverlayTest {
    private lateinit var activity: Activity
    private lateinit var root: FrameLayout
    private lateinit var overlay: ReaderOverlay
    private val opened = ArrayList<String>()
    private var gameTaps = 0
    private var secondScreen = false

    private val host = object : ReaderOverlay.Host {
        override fun openBacklog() { opened.add("backlog") }
        override fun openStatistics() { opened.add("statistics") }
        override fun openSettings() { opened.add("settings") }
        override fun openDictionaries() { opened.add("dictionaries") }
        override fun openAnkiSetup() { opened.add("anki") }
        override fun chooseCardAudio() { opened.add("card audio") }
        override fun hasSecondaryScreen() = secondScreen
    }

    // A made-up line of two rows, with furigana as a game would supply it.
    private val line = TextLine(
        sequence = 1, frame = 1, audioPosition = 0, channel = "dialogue", speaker = "声",
        text = "…君は旅人だ？\n道を見張れ",
        ruby = listOf(RubySpan(1, 1, "きみ"), RubySpan(3, 2, "たびびと"), RubySpan(8, 1, "みち"), RubySpan(10, 2, "みは")),
        sourceId = "a", meta = emptyMap(), timestamp = 1
    )

    private fun presented(l: TextLine = line, reread: Boolean = false) =
        PresentedLine(l, LineAccounting(12, reread, true, 1), 1)

    @Before
    fun setUp() {
        activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        // Of the reader's taps alone: touch play has tests of its own (ScreenTextOverlayTest).
        androidx.preference.PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putBoolean(ReaderSettings.KEY_TOUCH_PLAY, false).commit()
        activity.setTheme(com.google.android.material.R.style.Theme_Material3_DayNight_NoActionBar)
    }

    private fun build(width: Int, height: Int, secondary: Boolean = false) {
        root = FrameLayout(activity)
        // Stands in for the game picture and its touch handling.
        val game = View(activity).apply {
            setBackgroundColor(0xFF35506B.toInt())
            setOnClickListener { gameTaps++ }
        }
        root.addView(game, FrameLayout.LayoutParams(-1, -1))
        overlay = ReaderOverlay(activity, host, secondary)
        root.addView(overlay)
        activity.setContentView(root, ViewGroup.LayoutParams(width, height))
        layout(width, height)
    }

    private fun layout(width: Int = root.width, height: Int = root.height) {
        idle()
        applyFont(root)
        root.measure(
            View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY)
        )
        root.layout(0, 0, width, height)
        idle()
        root.measure(
            View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY)
        )
        root.layout(0, 0, width, height)
    }

    private fun idle() = shadowOf(Looper.getMainLooper()).idle()

    private val font: Typeface? by lazy {
        System.getenv("READER_PREVIEW_FONT")?.let(::File)?.takeIf { it.exists() }
            ?.let { Typeface.createFromFile(it) }
    }

    private fun applyFont(view: View) {
        val typeface = font ?: return
        if (view is TextView) view.typeface = typeface
        if (view is ViewGroup) for (i in 0 until view.childCount) applyFont(view.getChildAt(i))
    }

    private fun snapshot(name: String) {
        val dir = System.getenv("READER_PREVIEW_DIR") ?: return
        val bitmap = Bitmap.createBitmap(root.width, root.height, Bitmap.Config.ARGB_8888)
        root.draw(Canvas(bitmap))
        File(dir).mkdirs()
        File(dir, "$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private inline fun <reified T : View> find(
        view: View = root,
        noinline test: (T) -> Boolean = { true }
    ): T? = findView(view, T::class.java, test)

    private fun <T : View> findView(view: View, type: Class<T>, test: (T) -> Boolean): T? {
        if (type.isInstance(view) && test(type.cast(view)!!)) return type.cast(view)
        if (view is ViewGroup) {
            for (i in 0 until view.childCount) findView(view.getChildAt(i), type, test)?.let { return it }
        }
        return null
    }

    private fun positionInRoot(view: View): Pair<Int, Int> {
        var x = 0
        var y = 0
        var current: View? = view
        while (current != null && current !== root) {
            x += current.left - current.scrollX
            y += current.top - current.scrollY
            current = current.parent as? View
        }
        return x to y
    }

    private fun tap(x: Float, y: Float) {
        val now = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x, y, 0)
        val up = MotionEvent.obtain(now, now + 40, MotionEvent.ACTION_UP, x, y, 0)
        root.dispatchTouchEvent(down)
        root.dispatchTouchEvent(up)
        down.recycle()
        up.recycle()
        idle()
    }

    /**
     * Centre of the character at [offset] of [view]'s text, in root coordinates, worked out from
     * the layout and glyph advances rather than from the view's own hit-testing. A run with
     * furigana is laid out as one box with its base text centred inside.
     */
    private fun centreOf(view: ReaderTextView, offset: Int, spans: List<RubySpan> = line.ruby): Pair<Float, Float> {
        val layout = view.layout
        val content = view.text
        val lineIndex = layout.getLineForOffset(offset)
        fun edge(position: Int) = if (position >= layout.getLineVisibleEnd(lineIndex)) {
            layout.getLineRight(lineIndex)
        } else {
            layout.getPrimaryHorizontal(position)
        }
        val span = spans.firstOrNull { offset >= it.start && offset < it.start + it.length }
        val x = if (span == null) {
            (edge(offset) + edge(offset + 1)) / 2f
        } else {
            val start = span.start
            val end = span.start + span.length
            val boxLeft = edge(start)
            val boxWidth = edge(end) - boxLeft
            val baseWidth = view.paint.measureText(content, start, end)
            boxLeft + (boxWidth - baseWidth) / 2f + view.paint.measureText(content, start, offset) +
                view.paint.measureText(content, offset, offset + 1) / 2f
        }
        val (vx, vy) = positionInRoot(view)
        val y = layout.getLineBaseline(lineIndex) - view.paint.textSize / 2f
        return (vx + view.totalPaddingLeft + x) to (vy + view.totalPaddingTop + y)
    }

    private fun installDictionary() {
        val store = DictionaryStore.get(activity)
        if (store.database.hasEnabledDictionaries()) return
        val zip = File(activity.cacheDir, "test-dict.zip")
        ZipOutputStream(zip.outputStream()).use { out ->
            out.putNextEntry(ZipEntry("index.json"))
            out.write("""{"title":"Test Dict","revision":"1","format":3}""".toByteArray())
            out.closeEntry()
            out.putNextEntry(ZipEntry("term_bank_1.json"))
            out.write(
                """[["見張る","みはる","v5","v5",10,["to keep watch","to stand guard"],1,""],
                    ["見","けん","n","",5,["view"],2,""],
                    ["旅人","たびびと","n","",5,["traveller","wanderer"],3,""],
                    ["君","きみ","pn","",5,["you","buddy"],4,""]]""".toByteArray()
            )
            out.closeEntry()
        }
        DictionaryImporter(store.database).import(zip)
    }

    private fun awaitPopup(): LookupPopupView {
        val deadline = System.currentTimeMillis() + 5000
        while (System.currentTimeMillis() < deadline) {
            idle()
            find<LookupPopupView> { it.visibility == View.VISIBLE }?.let {
                layout()
                return it
            }
            Thread.sleep(20)
        }
        throw AssertionError("the dictionary popup did not open")
    }

    private fun allText(view: View): String = buildString {
        if (view is TextView) append(view.text).append('\n')
        if (view is ViewGroup) for (i in 0 until view.childCount) append(allText(view.getChildAt(i)))
    }

    @Test
    fun panelShowsTheLineAtTheBottomAndLeavesTheGameAlone() {
        build(1920, 1080)
        assertEquals(View.GONE, find<ReaderTextView>()!!.let { (it.parent.parent.parent as View).visibility })
        overlay.onLine(presented())
        layout()
        snapshot("panel-main-screen")

        val text = find<ReaderTextView>()!!
        assertEquals("…君は旅人だ？\n道を見張れ", text.text.toString())
        assertEquals(2, text.layout.lineCount)
        val (_, textY) = positionInRoot(text)
        assertTrue("the panel sits in the lower part of the screen", textY > 1080 / 2)
        assertTrue("the panel stays on screen", textY + text.height <= 1080)
        assertTrue(allText(root).contains("声"))

        // A tap on the picture above the panel goes to the game.
        tap(960f, 200f)
        assertEquals(1, gameTaps)
    }

    @Test
    fun theAddButtonStandsInTheCornerTheSettingsChoose() {
        installDictionary()
        val preferences = androidx.preference.PreferenceManager.getDefaultSharedPreferences(activity)
        val corners = listOf(null, "bottom_left", "bottom_right", "top_left", "top_right")
        for (corner in corners) {
            if (corner == null) preferences.edit().remove(ReaderSettings.KEY_CARD_CORNER).commit()
            else preferences.edit().putString(ReaderSettings.KEY_CARD_CORNER, corner).commit()
            build(1920, 1080)
            overlay.onLine(presented())
            layout()
            val text = find<ReaderTextView>()!!
            val (x, y) = centreOf(text, 10)
            tap(x, y)
            val popup = awaitPopup()
            snapshot("lookup-card-corner-${corner ?: "default"}")
            val label = activity.getString(R.string.reader_lookup_add)
            val add = find<android.widget.ImageButton> { it.contentDescription == label && it.isShown }!!
            // The headword's part the button belongs to: the frame that holds its row of boxes.
            val part = add.parent.parent as View
            val (bx, by) = positionInRoot(add)
            val (px, py) = positionInRoot(part)
            val centreX = bx + add.paddingLeft + (add.width - add.paddingLeft - add.paddingRight) / 2f
            val centreY = by + add.paddingTop + (add.height - add.paddingTop - add.paddingBottom) / 2f
            val left = centreX - px < part.width / 2f
            val top = centreY - py < part.height / 2f
            val wanted = corner ?: "bottom_left"
            assertEquals("$wanted: left", wanted.endsWith("left"), left)
            assertEquals("$wanted: top", wanted.startsWith("top"), top)
            // No character of the headline or the senses lies under the box.
            val boxLeft = bx + add.paddingLeft
            val boxRight = bx + add.width - add.paddingRight
            val boxTop = by + add.paddingTop
            val boxBottom = by + add.height - add.paddingBottom
            fun check(v: View) {
                if (v is TextView && v !is android.widget.ImageButton && v.isShown && v.layout != null) {
                    val (tx, ty) = positionInRoot(v)
                    val layout = v.layout
                    for (index in 0 until v.text.length) {
                        val c = v.text[index]
                        if (c == '\uFFFC' || c.isWhitespace()) continue
                        val line = layout.getLineForOffset(index)
                        val a = layout.getPrimaryHorizontal(index)
                        val b = if (index + 1 < layout.getLineEnd(line)) layout.getPrimaryHorizontal(index + 1) else layout.getLineRight(line)
                        val cl = tx + v.totalPaddingLeft + minOf(a, b)
                        val cr = tx + v.totalPaddingLeft + maxOf(a, b)
                        val ct = ty + v.totalPaddingTop + layout.getLineTop(line)
                        val cb = ty + v.totalPaddingTop + layout.getLineBottom(line)
                        val apart = cr <= boxLeft + 1 || cl >= boxRight - 1 || cb <= boxTop + 1 || ct >= boxBottom - 1
                        assertTrue("$wanted: '$c' under the box", apart)
                    }
                }
                if (v is ViewGroup) for (k in 0 until v.childCount) check(v.getChildAt(k))
            }
            check(part)
            tap(5f, 5f)
            idle()
        }
    }

    @Test
    fun tappingACharacterLooksUpTheWordStartingThere() {
        installDictionary()
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val text = find<ReaderTextView>()!!

        // Every character reports its own offset, including those under furigana.
        for (offset in line.text.indices) {
            if (line.text[offset] == '\n') continue
            val (x, y) = centreOf(text, offset)
            val (vx, vy) = positionInRoot(text)
            assertEquals("character ${line.text[offset]}", offset, text.characterAt(x - vx, y - vy))
        }

        // 見張れ starts at offset 10 on the second line.
        val (x, y) = centreOf(text, 10)
        tap(x, y)
        val popup = awaitPopup()
        snapshot("lookup-main-screen")
        val shown = allText(popup)
        assertTrue(shown, shown.contains("見張る"))
        assertTrue(shown, shown.contains("みはる"))
        assertTrue(shown, shown.contains("to keep watch"))
        assertTrue(shown, shown.contains("imperative"))
        assertTrue("the shorter word 見 is offered too", shown.contains("view"))
        assertEquals(0, gameTaps)

        // The matched word 見張れ (offsets 10 to 12) is marked in the panel.
        assertEquals(10..12, text.markedRange)

        // The popup is on screen, right next to the word: just above or just below its row.

        val (_, popupY) = positionInRoot(popup)

        val (_, textY) = positionInRoot(text)

        val row = text.boundsOf(10)

        assertTrue(popupY >= 0)

        val gapAbove = textY + row.top - (popupY + popup.height)

        val gapBelow = popupY - (textY + row.bottom)

        assertTrue("popup ($popupY, ${popup.height} high) is next to the word's row ($row)", gapAbove in 0..40 || gapBelow in 0..40)
    }

    @Test
    fun aTapOutsideClosesThePopupWithoutReachingTheGame() {
        installDictionary()
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val text = find<ReaderTextView>()!!
        val (x, y) = centreOf(text, 3)
        tap(x, y)
        val popup = awaitPopup()
        assertTrue(allText(popup).contains("旅人"))

        tap(30f, 30f) // far from both the popup and the panel
        assertEquals(View.GONE, popup.visibility)
        assertEquals(null, text.markedRange)
        assertEquals("closing the popup must not click into the game", 0, gameTaps)

        // With the popup closed, the same spot belongs to the game again.
        tap(30f, 30f)
        assertEquals(1, gameTaps)
    }

    @Test
    fun anotherWordCanBeLookedUpWithOneTapWhileThePopupIsOpen() {
        installDictionary()
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val text = find<ReaderTextView>()!!
        centreOf(text, 3).let { (x, y) -> tap(x, y) }
        assertTrue(allText(awaitPopup()).contains("旅人"))
        centreOf(text, 1).let { (x, y) -> tap(x, y) }
        Thread.sleep(200)
        val shown = allText(awaitPopup())
        assertTrue(shown, shown.contains("君"))
        assertTrue(shown, shown.contains("buddy"))
    }

    @Test
    fun aNewLineClosesThePopupAndReplacesTheText() {
        installDictionary()
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val text = find<ReaderTextView>()!!
        centreOf(text, 10).let { (x, y) -> tap(x, y) }
        val popup = awaitPopup()
        overlay.onLine(presented(line.copy(text = "私の名前は　まだない…", ruby = emptyList(), speaker = "少年")))
        layout()
        assertEquals(View.GONE, popup.visibility)
        assertEquals("私の名前は　まだない…", text.text.toString())
        assertTrue(allText(root).contains("少年"))
    }

    @Test
    fun withoutDictionariesThePopupSaysSoAndOffersTheManager() {
        DictionaryStore.get(activity).database.let { db -> db.dictionaries().forEach { db.delete(it.id) } }
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val text = find<ReaderTextView>()!!
        centreOf(text, 1).let { (x, y) -> tap(x, y) }
        val popup = awaitPopup()
        snapshot("lookup-no-dictionaries")
        assertTrue(allText(popup).contains("No dictionaries are installed"))
        val link = find<TextView>(popup) { it.text.toString() == "Open dictionaries" }
        assertNotNull(link)
        link!!.performClick()
        assertEquals(listOf("dictionaries"), opened)
    }

    @Test
    fun hidingThePanelLeavesAHandleAndGivesTheScreenBack() {
        build(1920, 1080)
        overlay.onLine(presented())
        layout()
        val hide = find<View> { it.contentDescription == "Hide the reader panel" }!!
        hide.performClick()
        layout()
        snapshot("panel-hidden")
        val text = find<ReaderTextView>()!!
        assertEquals(View.GONE, (text.parent.parent.parent as View).visibility)
        // Where the panel was is the game's again.
        tap(960f, 1000f)
        assertEquals(1, gameTaps)
        val show = find<View> { it.contentDescription == "Show the reader panel" && it.visibility == View.VISIBLE }
        assertNotNull(show)
        show!!.performClick()
        layout()
        assertEquals(View.VISIBLE, (text.parent.parent.parent as View).visibility)
    }

    @Test
    fun choicesAppearUnderTheLine() {
        build(1920, 1080)
        overlay.onLine(presented(line.copy(text = "「フリン」でよろしいですか？", ruby = emptyList(), speaker = "")))
        overlay.onLine(presented(line.copy(channel = "choice", text = "はい\nいいえ", ruby = emptyList(), speaker = "")))
        layout()
        snapshot("panel-choices")
        val views = ArrayList<ReaderTextView>()
        fun collect(v: View) {
            if (v is ReaderTextView) views.add(v)
            if (v is ViewGroup) for (i in 0 until v.childCount) collect(v.getChildAt(i))
        }
        collect(root)
        assertEquals("「フリン」でよろしいですか？", views[0].text.toString())
        assertEquals("はい\nいいえ", views[1].text.toString())
        assertEquals(View.VISIBLE, views[1].visibility)
    }

    @Test
    @Config(qualifiers = "w472dp-h411dp-420dpi")
    fun onADualScreenDeviceThePanelMovesToTheSecondScreen() {
        // The overlay on the main screen stays empty while a second screen is in use...
        secondScreen = true
        build(1920, 1080, secondary = false)
        overlay.onLine(presented())
        layout()
        assertEquals(View.GONE, find<ReaderTextView>()!!.let { (it.parent.parent.parent as View).visibility })

        // ...and the one on the second screen shows the line.
        build(1240, 1080, secondary = true)
        overlay.onLine(presented())
        layout()
        snapshot("panel-second-screen")
        val text = find<ReaderTextView>()!!
        assertEquals(View.VISIBLE, (text.parent.parent.parent as View).visibility)
        val (textX, textY) = positionInRoot(text)
        assertTrue(textX >= 0 && textX + text.width <= 1240)
        assertTrue(textY + text.height <= 1080)
    }

    @Test
    fun aLongLineScrollsInsideThePanelInsteadOfCoveringTheScreen() {
        build(1920, 1080)
        val long = "これは長い文章です。".repeat(120)
        overlay.onLine(presented(line.copy(text = long, ruby = emptyList())))
        layout()
        snapshot("panel-long-line")
        val text = find<ReaderTextView>()!!
        val panel = text.parent.parent.parent as View
        assertTrue("panel height ${panel.height}", panel.height <= 1080 * 38 / 100)
        assertTrue(text.height > panel.height) // the text itself is taller and scrolls
    }
}
