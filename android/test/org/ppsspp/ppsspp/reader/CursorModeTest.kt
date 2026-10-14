// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import org.ppsspp.ppsspp.reader.ui.LookupLayer
import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
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
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.cursor.HoverCursor
import org.ppsspp.ppsspp.reader.dict.DictionaryImporter
import org.ppsspp.ppsspp.reader.dict.DictionaryStore
import org.ppsspp.ppsspp.reader.mining.Miner
import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.screen.Area
import org.ppsspp.ppsspp.reader.screen.ScreenGeometry
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.ui.LookupPopupView
import org.ppsspp.ppsspp.reader.ui.ReaderOverlay
import org.ppsspp.ppsspp.reader.ui.ReaderStyle
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
import org.robolectric.shadows.ShadowToast

/**
 * The cursor: while its button is held, the trackpad area is a trackpad, the sticks
 * steer, and the word the cursor rests on is looked up. The real overlay views take real touch
 * events here; buttons and sticks arrive the way the emulation screen passes them on.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = "w834dp-h469dp-369dpi") // an AYN Thor's upper display: 1920x1080
class CursorModeTest {
    private lateinit var activity: Activity
    private lateinit var root: FrameLayout
    private lateinit var overlay: ReaderOverlay
    private lateinit var layer: ScreenTextLayer
    private lateinit var game: GamePicture
    private var gameTouches = 0

    /** What the emulator was told about the sticks: taken from the game (true) or given back. */
    private val told = ArrayList<Boolean>()

    private val host = object : ReaderOverlay.Host {
        override fun openBacklog() {}
        override fun openStatistics() {}
        override fun openSettings() {}
        override fun openDictionaries() {}
        override fun openAnkiSetup() {}
        override fun chooseCardAudio() {}
        override fun hasSecondaryScreen() = false
    }

    // One surface with the game's picture large on the left and a trackpad area beside it.
    private val topArea = Area(0f, 115f, 1416f, 965f)
    private val bottomArea = Area(1416f, 115f, 1920f, 493f)
    private val both = ScreenGeometry(1920, 1080, listOf(topArea), listOf(bottomArea))

    // The emulator's numbers for buttons and sticks.
    private val zl = HoverCursor.BUTTON_L2
    private val buttonA = 0
    private val buttonR = 103 // R1, as Android numbers it
    private val leftStick = 0
    private val rightStick = 1

    private val text = "今日は良い天気だ\n散歩に行こう"

    private fun line(id: Long, window: String = "w") = PresentedLine(
        TextLine(
            sequence = id, frame = 1, audioPosition = 0, channel = "dialogue", speaker = "",
            text = text, ruby = emptyList(), sourceId = "made-up-$id", meta = emptyMap(), timestamp = 1,
            window = window
        ),
        LineAccounting(14, false, true, 1),
        id
    )

    /** Two rows from x 40, 16 pixels a character, at y 191 and 217: a message window. */
    private fun layoutOf(id: Long) = ScreenLayout.fromJson(
        buildString {
            append("""{"window":"w","seq":$id,"space":[400,240],"box":[0,179,400,61],"glyphs":[""")
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

    private val layout = layoutOf(7)

    private fun scaleX(x: Float) = topArea.left + x * topArea.width / 400f
    private fun scaleY(y: Float) = topArea.top + y * topArea.height / 240f

    /** Centre of the character at [offset] of the text, where the game draws it. */
    private fun centreOf(offset: Int): Pair<Float, Float> {
        val cell = layout.cells[offset]!!
        return scaleX(cell.x + cell.width / 2) to scaleY(cell.y + cell.height / 2)
    }

    /** Stands in for the game: draws its two screens and counts the touches that reach it. */
    private inner class GamePicture(context: Context) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

        override fun onDraw(canvas: Canvas) {
            canvas.drawColor(0xFF2B3A4A.toInt())
            paint.color = 0xFF6C8FA8.toInt()
            canvas.drawRect(bottomArea.left, bottomArea.top, bottomArea.right, bottomArea.bottom, paint)
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

    @Before
    fun setUp() {
        TestSupport.resetStores()
        HoverCursor.reset()
        HoverCursor.armed = { true }
        HoverCursor.holdSticks = { held -> told.add(held) }
        HoverCursor.watchButton = {}
        activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(com.google.android.material.R.style.Theme_Material3_DayNight_NoActionBar)
        root = FrameLayout(activity)
        game = GamePicture(activity).apply {
            setOnTouchListener { _, _ ->
                gameTouches++
                true
            }
        }
        root.addView(game, FrameLayout.LayoutParams(-1, -1))
        overlay = ReaderOverlay(activity, host, secondary = false).also { it.gameSurface = game }
        root.addView(overlay)
        activity.setContentView(root, ViewGroup.LayoutParams(1920, 1080))
        layer = TestSupport.find(overlay, ScreenTextLayer::class.java)!!
        layer.geometrySource = { both }
        layout()
    }

    @After
    fun tearDown() {
        HoverCursor.reset()
        Miner.forget()
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

    /** Lets [ms] pass on the main thread's clock, running what falls due. */
    private fun pass(ms: Long) = shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))

    private fun showInGame(shown: PresentedLine = line(7), at: ScreenLayout = layout) {
        overlay.onLine(shown)
        ReaderSession.screen.adoptLine(shown)
        ReaderSession.screen.applyLayout(at)
        overlay.onScreenTextChanged()
        layout()
    }

    private fun touch(action: Int, x: Float, y: Float, downTime: Long, at: Long) {
        val event = MotionEvent.obtain(downTime, at, action, x, y, 0)
        root.dispatchTouchEvent(event)
        event.recycle()
        TestSupport.idle()
    }

    /** A finger drawn from one point to another in a few steps. */
    private fun drag(fromX: Float, fromY: Float, toX: Float, toY: Float) {
        val start = SystemClock.uptimeMillis()
        touch(MotionEvent.ACTION_DOWN, fromX, fromY, start, start)
        for (step in 1..4) {
            touch(
                MotionEvent.ACTION_MOVE,
                fromX + (toX - fromX) * step / 4f,
                fromY + (toY - fromY) * step / 4f,
                start,
                start + step * 20
            )
        }
        touch(MotionEvent.ACTION_UP, toX, toY, start, start + 400)
    }

    private fun tap(x: Float, y: Float) {
        val start = SystemClock.uptimeMillis()
        touch(MotionEvent.ACTION_DOWN, x, y, start, start)
        touch(MotionEvent.ACTION_UP, x, y, start, start + 40)
    }

    private fun hold() = HoverCursor.onButton(zl, true, HoverCursor.SOURCE_KEY)
    private fun letGo() = HoverCursor.onButton(zl, false, HoverCursor.SOURCE_KEY)

    /** Moves the cursor to a point the way a finger or a stick would: by a distance. */
    private fun bringCursorTo(x: Float, y: Float) = HoverCursor.moveBy(x - HoverCursor.x, y - HoverCursor.y)

    private fun picture(): Bitmap {
        layout()
        val bitmap = Bitmap.createBitmap(1920, 1080, Bitmap.Config.ARGB_8888)
        root.draw(Canvas(bitmap))
        return bitmap
    }

    /** What one view draws, on nothing. */
    private fun pictureOf(view: View): Bitmap {
        layout()
        val bitmap = Bitmap.createBitmap(1920, 1080, Bitmap.Config.ARGB_8888)
        view.draw(Canvas(bitmap))
        return bitmap
    }

    private fun installDictionary() {
        val store = DictionaryStore.get(activity)
        val zip = File(activity.cacheDir, "cursor-dict.zip")
        ZipOutputStream(zip.outputStream()).use { out ->
            out.putNextEntry(ZipEntry("index.json"))
            out.write("""{"title":"Test Dict","revision":"1","format":3}""".toByteArray())
            out.closeEntry()
            out.putNextEntry(ZipEntry("term_bank_1.json"))
            out.write(
                """[["天気","てんき","n","",10,["weather"],1,""],
                    ["散歩","さんぽ","n vs","vs",9,["walk","stroll"],2,""],
                    ["今日","きょう","n","",7,["today"],4,""]]""".toByteArray()
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

    @Test
    fun holdingTheButtonMakesTheBottomScreenATrackpad() {
        showInGame()
        val before = picture()

        // Nothing is held: sticks and other buttons are the game's, and so is the trackpad area.
        assertFalse(HoverCursor.onStick(leftStick, 1f, 0f))
        assertFalse(HoverCursor.onButton(buttonA, true, HoverCursor.SOURCE_KEY))
        tap(1600f, 300f)
        assertEquals(2, gameTouches)
        assertTrue(overlay.trackpad.isEmpty())

        assertTrue("the button is the reader's, not the game's", hold())
        assertTrue(HoverCursor.active)
        assertEquals("the sticks were taken from the game", listOf(true), told)
        assertEquals(listOf(bottomArea), overlay.trackpad)
        assertFalse(HoverCursor.everywhere)

        // The cursor waits at the start of the text, just before its first character.
        val first = layer.texts.single().characters.first()
        assertEquals(first.touch.left - 1f, HoverCursor.x, 0.5f)
        assertEquals((first.drawn.top + first.drawn.bottom) / 2f, HoverCursor.y, 0.5f)

        // The trackpad is dimmed, the picture is not, and the cursor is drawn.
        val held = picture()
        TestSupport.snapshot(root, 1920, 1080, "cursor-trackpad")
        assertTrue(
            "trackpad darker",
            Color.green(held.getPixel(1700, 300)) < Color.green(before.getPixel(1700, 300)) - 40
        )
        assertEquals("picture as it was", before.getPixel(700, 400), held.getPixel(700, 400))
        val ringX = (HoverCursor.x + 9 * activity.resources.displayMetrics.density).toInt()
        assertTrue(
            "a cursor where the reader says it is",
            held.getPixel(ringX, HoverCursor.y.toInt()) != before.getPixel(ringX, HoverCursor.y.toInt())
        )

        // A finger drawn across the trackpad moves the cursor: across the whole trackpad
        // screen would be across the whole display. None of it reaches the game.
        val x = HoverCursor.x
        val y = HoverCursor.y
        drag(1500f, 300f, 1600f, 310f)
        val gain = 1920f / bottomArea.width
        assertEquals(x + 100f * gain, HoverCursor.x, 1f)
        assertEquals(y + 10f * gain, HoverCursor.y, 1f)
        assertEquals("the trackpad's touches are not the game's", 2, gameTouches)

        // The picture is not a trackpad: a touch there is what it always was.
        tap(700f, 400f)
        assertEquals(4, gameTouches)

        // Let go: no trackpad, no dimming, no cursor, and the trackpad area is the game's again.
        assertTrue("letting go is not the game's business either", letGo())
        assertFalse(HoverCursor.active)
        assertTrue(overlay.trackpad.isEmpty())
        val after = picture()
        assertEquals(before.getPixel(1700, 300), after.getPixel(1700, 300))
        tap(1600f, 300f)
        assertEquals(6, gameTouches)
    }

    @Test
    fun theWordTheCursorRestsOnIsLookedUp() {
        installDictionary()
        showInGame()
        val plain = picture()
        hold()
        assertEquals("nothing is looked up before the cursor is on a word", View.GONE, popup().visibility)

        // Onto 天 of 天気 (offset 5): nothing at once, the word after a moment's rest.
        centreOf(5).let { (x, y) -> bringCursorTo(x, y) }
        assertEquals(View.GONE, popup().visibility)

        // On a character the cursor is a frame around it, and nothing is drawn over it.
        val cell = layer.texts.single().characters.first { it.offset == 5 }.drawn
        val framed = picture()
        val frameX = (cell.left - activity.resources.displayMetrics.density).toInt()
        val middleY = ((cell.top + cell.bottom) / 2).toInt()
        assertTrue("a frame at the character's edge", framed.getPixel(frameX, middleY) != plain.getPixel(frameX, middleY))
        // (Its line lies on the cell's edge; a glyph does not reach that far.)
        for (x in cell.left.toInt() + 5 until cell.right.toInt() - 5 step 3) {
            assertEquals("the character itself is untouched", plain.getPixel(x, middleY), framed.getPixel(x, middleY))
        }
        pass(200)
        val popup = awaitPopup("weather")
        TestSupport.snapshot(root, 1920, 1080, "cursor-lookup")
        val mark = layer.marks.single()
        assertEquals(scaleX(120f), mark.left, 0.5f)
        assertEquals(scaleX(152f), mark.right, 0.5f)

        // Swept across other characters without stopping: they are not looked up.
        centreOf(0).let { (x, y) -> bringCursorTo(x, y) }
        pass(60)
        bringCursorTo(scaleX(300f), scaleY(201f)) // in the text box, on no character
        pass(300)
        assertTrue(TestSupport.allText(popup).contains("weather"))

        // On to another word.
        centreOf(9).let { (x, y) -> bringCursorTo(x, y) }
        pass(200)
        assertTrue(TestSupport.allText(awaitPopup("stroll")).contains("さんぽ"))

        // Resting on a character that begins no word (は here) says nothing and changes
        // nothing: what was found before stays to be read.
        centreOf(2).let { (x, y) -> bringCursorTo(x, y) }
        pass(200)
        Thread.sleep(300)
        pass(50)
        assertEquals(View.VISIBLE, popup.visibility)
        assertTrue(TestSupport.allText(popup), TestSupport.allText(popup).contains("stroll"))
        assertFalse(TestSupport.allText(popup).contains("No entry"))

        // Over the popup itself the cursor looks nothing up.
        bringCursorTo(popup.left + popup.width / 2f, popup.top + popup.height / 2f)
        pass(300)
        assertTrue(TestSupport.allText(popup).contains("stroll"))

        // Letting go leaves what was found to be read.
        letGo()
        layout()
        assertEquals(View.VISIBLE, popup.visibility)
        assertEquals("the game was never touched", 0, gameTouches)
    }

    @Test
    fun thePopupClosesWhenLeftAloneAndIfWantedWhenTheCursorLeaves() {
        val prefs = PreferenceManager.getDefaultSharedPreferences(activity)
        installDictionary()
        showInGame()
        hold()
        fun open(): LookupPopupView {
            bringCursorTo(scaleX(300f), scaleY(201f)) // in the text box, on no character
            pass(100)
            centreOf(5).let { (x, y) -> bringCursorTo(x, y) }
            pass(200)
            return awaitPopup("weather")
        }
        var popup = open()

        // The cursor rests on the word: the popup stays, however long.
        pass(40_000)
        assertEquals(View.VISIBLE, popup.visibility)
        // Off the word it has its time (15 seconds unless set otherwise), then it goes.
        bringCursorTo(scaleX(300f), scaleY(201f))
        pass(14_000)
        assertEquals(View.VISIBLE, popup.visibility)
        pass(2_000)
        assertEquals("left alone for its time", View.GONE, popup.visibility)
        assertTrue("its mark goes with it", layer.marks.isEmpty())

        // Resting on the popup itself keeps it, and moving on it starts its time over.
        popup = open()
        bringCursorTo(popup.left + popup.width / 2f, popup.top + popup.height / 2f)
        pass(40_000)
        assertEquals(View.VISIBLE, popup.visibility)

        // Set to 0 it stays until it is closed.
        prefs.edit().putInt(ReaderSettings.KEY_POPUP_AUTOHIDE, 0).commit()
        TestSupport.idle()
        popup = open()
        bringCursorTo(scaleX(300f), scaleY(201f))
        pass(120_000)
        assertEquals(View.VISIBLE, popup.visibility)

        // As a setting it goes when the cursor leaves the word and the popup: not at once,
        // the way from the word to the popup crosses neither.
        prefs.edit().putBoolean(ReaderSettings.KEY_POPUP_HIDE_ON_EXIT, true).commit()
        TestSupport.idle()
        popup = open()
        bringCursorTo(scaleX(300f), scaleY(201f))
        pass(200)
        assertEquals("on its way to the popup", View.VISIBLE, popup.visibility)
        bringCursorTo(popup.left + popup.width / 2f, popup.top + popup.height / 2f)
        pass(2_000)
        assertEquals("on the popup", View.VISIBLE, popup.visibility)
        bringCursorTo(scaleX(300f), scaleY(201f))
        pass(LookupLayer.EXIT_GRACE_MS + 100)
        assertEquals("left for good", View.GONE, popup.visibility)

        letGo()
        prefs.edit().clear().commit()
        TestSupport.idle()
        assertEquals("the game was never touched", 0, gameTouches)
    }

    @Test
    fun theSticksMoveTheCursorWhileItIsHeld() {
        showInGame()
        hold()
        bringCursorTo(100f, 500f)

        // Pushed all the way: across the display in about a second.
        assertTrue(HoverCursor.onStick(leftStick, 1f, 0f))
        pass(500)
        val far = HoverCursor.x - 100f
        assertTrue("half a second at full push: $far", far in 850f..1000f)
        assertEquals(500f, HoverCursor.y, 0.5f)

        // Centred again, the cursor stays.
        assertTrue(HoverCursor.onStick(leftStick, 0f, 0f))
        val rested = HoverCursor.x
        pass(300)
        assertEquals(rested, HoverCursor.x, 0.5f)

        // Pushed a little, it creeps: that is for the last few pixels onto a character.
        HoverCursor.onStick(rightStick, 0f, -0.3f)
        pass(500)
        val crept = 500f - HoverCursor.y
        assertTrue("half a second at a light push: $crept", crept in 60f..200f)
        assertEquals(rested, HoverCursor.x, 0.5f)
        HoverCursor.onStick(rightStick, 0f, 0f)

        // It cannot leave the display.
        HoverCursor.onStick(leftStick, -1f, -1f)
        pass(3000)
        assertEquals(0f, HoverCursor.x, 0.5f)
        assertEquals(0f, HoverCursor.y, 0.5f)

        // Let go with the stick still pushed: the cursor stops, and the sticks are the game's
        // again, as they are.
        told.clear()
        letGo()
        pass(300)
        assertEquals(0f, HoverCursor.x, 0.5f)
        assertEquals(listOf(false), told)
        assertFalse(HoverCursor.onStick(leftStick, 1f, 0f))
    }

    @Test
    fun aTapOnTheTrackpadClicksWhereTheCursorIs() {
        installDictionary()
        showInGame()
        hold()
        centreOf(5).let { (x, y) -> bringCursorTo(x, y) }
        pass(200)
        val popup = awaitPopup("weather")

        // The cursor on the popup's box, then a tap on the trackpad: the box is pressed. No
        // Anki is set up here, which is what it says.
        val add = TestSupport.find(popup, ImageButton::class.java) { it.contentDescription == "Add to Anki" }!!
        val place = IntArray(2).also { add.getLocationInWindow(it) }
        val origin = IntArray(2).also { overlay.getLocationInWindow(it) }
        bringCursorTo(
            place[0] - origin[0] + add.width - 20f,
            place[1] - origin[1] + add.height - 20f
        )
        tap(1600f, 300f)
        TestSupport.await("the box to answer") { ShadowToast.getTextOfLatestToast() != null }
        assertTrue(ShadowToast.getTextOfLatestToast().startsWith("Card not added"))
        assertEquals(View.VISIBLE, popup.visibility)

        // A tap with the cursor on nothing closes the popup, as a tap beside it would.
        bringCursorTo(200f, 60f)
        tap(1600f, 300f)
        assertEquals(View.GONE, popup.visibility)

        // A finger that stays long does not click, nor does one that moves.
        centreOf(9).let { (x, y) -> bringCursorTo(x, y) }
        pass(200)
        awaitPopup("stroll")
        bringCursorTo(200f, 60f)
        val start = SystemClock.uptimeMillis()
        touch(MotionEvent.ACTION_DOWN, 1600f, 300f, start, start)
        touch(MotionEvent.ACTION_UP, 1600f, 300f, start, start + 800)
        assertEquals(View.VISIBLE, popup.visibility)
        drag(1600f, 300f, 1650f, 300f)
        assertEquals(View.VISIBLE, popup.visibility)

        // A tap that wobbles a little is still a tap, and clicks where the cursor was put.
        bringCursorTo(200f, 60f)
        val again = SystemClock.uptimeMillis()
        touch(MotionEvent.ACTION_DOWN, 1600f, 300f, again, again)
        touch(MotionEvent.ACTION_MOVE, 1604f, 303f, again, again + 20)
        touch(MotionEvent.ACTION_UP, 1604f, 303f, again, again + 60)
        assertEquals(View.GONE, popup.visibility)
        assertEquals(200f, HoverCursor.x, 0.5f)
        assertEquals(60f, HoverCursor.y, 0.5f)
        assertEquals(0, gameTouches)
    }

    @Test
    fun aNewLineHasTheCursorAtItsStartAgain() {
        showInGame()
        hold()
        val first = layer.texts.single().characters.first()
        val homeX = first.touch.left - 1f
        bringCursorTo(900f, 300f)

        // Let go and hold again on the same line: the cursor is where it was left.
        letGo()
        hold()
        assertEquals(900f, HoverCursor.x, 0.5f)
        assertEquals(300f, HoverCursor.y, 0.5f)

        // The game moves on while the button is held: back to the start of the new text.
        showInGame(line(8), layoutOf(8))
        assertEquals(homeX, HoverCursor.x, 0.5f)
        assertEquals(8L, HoverCursor.homedFor)
    }

    @Test
    fun asASettingTheCursorStaysARingOverTheText() {
        installDictionary()
        showInGame()
        val plain = picture()
        hold()
        centreOf(5).let { (x, y) -> bringCursorTo(x, y) }
        val cell = layer.texts.single().characters.first { it.offset == 5 }.drawn
        val density = activity.resources.displayMetrics.density
        val frameX = (cell.left - density).toInt()
        val middleY = ((cell.top + cell.bottom) / 2).toInt()
        val ringX = (HoverCursor.x + 9 * density).toInt()

        // Unless chosen otherwise the cursor is a frame around the character it is on.
        assertTrue(ReaderSettings(activity).cursorFrame)
        val framed = picture()
        assertTrue("a frame at the character's edge", framed.getPixel(frameX, middleY) != plain.getPixel(frameX, middleY))
        assertEquals("and no ring", plain.getPixel(ringX, middleY), framed.getPixel(ringX, middleY))

        // Switched off, it is the pointer it is everywhere else, at the point where it is.
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putBoolean(ReaderSettings.KEY_CURSOR_FRAME, false).commit()
        val free = picture()
        // The pointer is the console's, wide enough to reach over the character's edge: the
        // picture is no longer the frame's, and around the point no longer the game's.
        assertTrue("not the frame", !free.sameAs(framed))
        val reach = (maxOf(30 * density, root.height / 20f) * 0.44f).toInt()
        assertTrue("the pointer around the point", (-reach..reach).any { dx ->
            (-reach..reach).any { dy ->
                val x = HoverCursor.x.toInt() + dx
                val y = HoverCursor.y.toInt() + dy
                free.getPixel(x, y) != plain.getPixel(x, y)
            }
        })
        TestSupport.snapshot(root, 1920, 1080, "cursor-ring-on-text")

        // The word it rests on is looked up all the same, and marked in the game's text.
        pass(200)
        awaitPopup("weather")
        assertEquals(scaleX(120f), layer.marks.single().left, 0.5f)
    }

    @Test
    fun asASettingTheCursorStaysWhereItWasLeftWhenANewLineComes() {
        installDictionary()
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putBoolean(ReaderSettings.KEY_CURSOR_HOME, false).commit()
        assertFalse(ReaderSettings(activity).cursorHome)
        showInGame()

        // It has never been anywhere: it starts in the middle of the game's picture, not at the text.
        hold()
        assertEquals((topArea.left + topArea.right) / 2f, HoverCursor.x, 0.5f)
        assertEquals((topArea.top + topArea.bottom) / 2f, HoverCursor.y, 0.5f)

        // Moved onto 天 of 天気, it looks the word up as ever.
        centreOf(5).let { (x, y) -> bringCursorTo(x, y) }
        pass(200)
        awaitPopup("weather")
        val x = HoverCursor.x
        val y = HoverCursor.y

        // The game moves on while the button is held. The cursor stays where it is, and what is
        // under it now (the same words, as it happens) is not looked up: nobody pointed at it.
        showInGame(line(8), layoutOf(8))
        assertEquals(x, HoverCursor.x, 0.5f)
        assertEquals(y, HoverCursor.y, 0.5f)
        pass(400)
        Thread.sleep(200)
        pass(50)
        assertEquals("the new line closed the popup and nothing opened one", View.GONE, popup().visibility)

        // The slightest move is pointing: the word under the cursor is looked up.
        HoverCursor.moveBy(2f, 0f)
        pass(200)
        awaitPopup("weather")

        // Let go, and hold again on the next line: where it was left, and quiet until it is moved.
        letGo()
        showInGame(line(9), layoutOf(9))
        assertEquals(View.GONE, popup().visibility)
        hold()
        assertEquals(x + 2f, HoverCursor.x, 0.5f)
        assertEquals(y, HoverCursor.y, 0.5f)
        pass(400)
        Thread.sleep(200)
        pass(50)
        assertEquals(View.GONE, popup().visibility)
        HoverCursor.moveBy(-2f, 0f)
        pass(200)
        awaitPopup("weather")

        // A tap on the trackpad clicks where the cursor is, moved or not.
        showInGame(line(10), layoutOf(10))
        assertEquals(View.GONE, popup().visibility)
        tap(1600f, 300f)
        awaitPopup("weather")

        // Switched back, the next line has the cursor at its start again.
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putBoolean(ReaderSettings.KEY_CURSOR_HOME, true).commit()
        showInGame(line(11), layoutOf(11))
        val first = layer.texts.single().characters.first()
        assertEquals(first.touch.left - 1f, HoverCursor.x, 0.5f)
        assertEquals(11L, HoverCursor.homedFor)
    }

    @Test
    fun asASettingOnePressSwitchesTheCursorOnAndTheNextSwitchesItOff() {
        showInGame()
        assertFalse("holding is how it works unless chosen otherwise", ReaderSettings(activity).cursorToggle)
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putBoolean(ReaderSettings.KEY_CURSOR_TOGGLE, true).commit()
        val settings = ReaderSettings(activity)
        HoverCursor.configure(settings.cursorButton, settings.cursorSpeed, settings.cursorToggle)

        // Pressed and let go: the cursor is on, and stays on.
        assertTrue(hold())
        assertTrue(letGo())
        assertTrue(HoverCursor.active)
        assertEquals(listOf(bottomArea), overlay.trackpad)
        val x = HoverCursor.x
        drag(1500f, 300f, 1600f, 300f)
        assertEquals(x + 100f * 1920f / bottomArea.width, HoverCursor.x, 1f)
        assertTrue("the sticks are the cursor's as long as it is on", HoverCursor.onStick(leftStick, 0f, 0f))
        assertEquals(0, gameTouches)

        // A key that is held repeats, and a trigger's axis says "pressed" again with every
        // movement of a stick: neither is a second press.
        hold()
        hold()
        hold()
        assertFalse("the second press switched it off", HoverCursor.active)
        letGo()
        assertFalse(HoverCursor.active)
        assertTrue(overlay.trackpad.isEmpty())
        assertFalse(HoverCursor.onStick(leftStick, 1f, 0f))
        hold()
        hold()
        assertTrue("and the third on again, however often the press is repeated", HoverCursor.active)
        letGo()

        // The key bound in the emulator's hotkey settings switches it the same way.
        HoverCursor.onHotkey(true)
        HoverCursor.onHotkey(true)
        HoverCursor.onHotkey(false)
        assertFalse(HoverCursor.active)
        HoverCursor.onHotkey(true)
        HoverCursor.onHotkey(false)
        assertTrue(HoverCursor.active)

        // A paused game switches it off; so does going back to holding.
        HoverCursor.cancel()
        assertFalse(HoverCursor.active)
        hold()
        letGo()
        assertTrue(HoverCursor.active)
        HoverCursor.configure(settings.cursorButton, settings.cursorSpeed, false)
        assertFalse(HoverCursor.active)
        assertTrue(hold())
        assertTrue(HoverCursor.active)
        letGo()
        assertFalse("held again: letting go is the end of it", HoverCursor.active)
        PreferenceManager.getDefaultSharedPreferences(activity).edit().clear().commit()
    }

    @Test
    fun onTwoDisplaysTheSecondIsTheTrackpadAllTheTimeAndTakesNothingFromTheGame() {
        // The main display shows the game's picture; a second overlay stands for the other
        // display's, which is all trackpad (laid over the first here: only its own
        // screen area matters).
        layer.geometrySource = { ScreenGeometry(1920, 1080, listOf(Area(60f, 0f, 1860f, 1080f)), emptyList()) }
        val lower = ReaderOverlay(activity, host, secondary = true)
        root.addView(lower)
        val pad = Area(0f, 130f, 1240f, 1005f)
        TestSupport.find(lower, ScreenTextLayer::class.java)!!.geometrySource =
            { ScreenGeometry(1240, 1080, emptyList(), listOf(pad)) }
        layout()

        // No button was pressed: the trackpad is there because there is text to point at.
        assertTrue(HoverCursor.always)
        assertTrue(HoverCursor.active)
        assertFalse(HoverCursor.everywhere)
        assertTrue("the main display has no trackpad", overlay.trackpad.isEmpty())
        assertEquals(listOf(pad), lower.trackpad)

        // The controller is the game's, all of it: the cursor's button, the sticks, and with
        // them the d-pad, which a controller reports beside the sticks.
        assertFalse("the sticks were never taken", told.contains(true))
        assertFalse(HoverCursor.takesSticks)
        assertFalse("the button is the game's", hold())
        assertFalse(letGo())
        val x = HoverCursor.x
        assertFalse("a stick is the game's", HoverCursor.onStick(0, 1f, 0f))
        pass(300)
        assertEquals("and moves nothing", x, HoverCursor.x, 0f)
        assertFalse(told.contains(true))

        drag(300f, 500f, 500f, 500f)
        assertEquals("across the lower screen is across the upper", x + 200f * 1920f / 1240f, HoverCursor.x, 1f)
        assertEquals(0, gameTouches)

        // Nothing to read any more: no trackpad, and the button is what it was.
        HoverCursor.armed = { false }
        HoverCursor.refresh()
        assertFalse(HoverCursor.always)
        assertFalse(HoverCursor.active)
        assertTrue(lower.trackpad.isEmpty())
        HoverCursor.armed = { true }
        HoverCursor.refresh()
        assertTrue(HoverCursor.always)

        // The second display goes away: the button holds the cursor again.
        root.removeView(lower)
        assertFalse(HoverCursor.always)
        assertFalse(HoverCursor.active)
        assertTrue(hold())
        assertTrue(HoverCursor.active)
        assertTrue(HoverCursor.takesSticks)
        letGo()
    }

    @Test
    fun theSecondDisplayLooksLikeTheConsoleWhileItIsTheTrackpad() {
        layer.geometrySource = { ScreenGeometry(1920, 1080, listOf(Area(60f, 0f, 1860f, 1080f)), emptyList()) }
        val lower = ReaderOverlay(activity, host, secondary = true)
        root.addView(lower)
        val pad = Area(0f, 0f, 1240f, 1080f)
        TestSupport.find(lower, ScreenTextLayer::class.java)!!.geometrySource =
            { ScreenGeometry(1240, 1080, emptyList(), listOf(pad)) }
        layout()
        val hint = lower.findViewById<TextView>(R.id.reader_pad_hint)
        assertNull("the game's own display says nothing of the kind", overlay.findViewById<View>(R.id.reader_pad_hint))

        // It says what it is, for a moment, and is drawn edge to edge in the console's look.
        assertEquals("Trackpad", hint.text.toString())
        assertEquals(1f, hint.alpha, 0f)
        val lit = pictureOf(lower)
        for ((px, py) in listOf(620 to 300, 60 to 60, 1180 to 1020)) {
            assertEquals("opaque at $px,$py", 255, Color.alpha(lit.getPixel(px, py)))
        }
        assertTrue("nothing of the main display is dimmed for it", overlay.trackpad.isEmpty())
        pass(10_000)
        assertEquals("the word does not stay", 0f, hint.alpha, 0f)
        assertEquals("the trackpad does", 255, Color.alpha(pictureOf(lower).getPixel(620, 300)))

        // A finger on it shows where it is.
        val plain = pictureOf(lower)
        touch(MotionEvent.ACTION_DOWN, 400f, 500f, 1000, 1000)
        assertFalse("the console's pointer under the finger", plain.sameAs(pictureOf(lower)))
        // With READER_PREVIEW_DIR set, a picture of it to look at.
        System.getenv("READER_PREVIEW_DIR")?.let { dir ->
            java.io.File(dir).mkdirs()
            java.io.FileOutputStream(java.io.File(dir, "trackpad.png")).use {
                pictureOf(lower).compress(Bitmap.CompressFormat.PNG, 100, it)
            }
        }
        touch(MotionEvent.ACTION_UP, 400f, 500f, 1000, 1900)

        // Nothing to read: dark, and no word.
        HoverCursor.armed = { false }
        HoverCursor.refresh()
        assertEquals(0f, hint.alpha, 0f)
        assertEquals(Color.TRANSPARENT, pictureOf(lower).getPixel(620, 300))
    }

    @Test
    fun withNoBottomScreenToBeSeenTheMainScreenIsTheTrackpad() {
        layer.geometrySource = { ScreenGeometry(1920, 1080, listOf(Area(60f, 0f, 1860f, 1080f)), emptyList()) }
        layout()
        val before = picture()
        hold()
        assertTrue(HoverCursor.everywhere)
        assertEquals(listOf(Area(0f, 0f, 1920f, 1080f)), overlay.trackpad)
        val x = HoverCursor.x
        val y = HoverCursor.y
        drag(400f, 400f, 500f, 440f)
        assertEquals("one for one", x + 100f, HoverCursor.x, 1f)
        assertEquals(y + 40f, HoverCursor.y, 1f)
        assertEquals(0, gameTouches)
        // Nothing is dimmed: there would be nothing left to read.
        assertEquals(before.getPixel(1700, 900), picture().getPixel(1700, 900))
    }

    @Test
    fun theButtonAndItsSpeedAreSettingsAndTheButtonIsTheGamesWhenThereIsNothingToRead() {
        val settings = ReaderSettings(activity)
        assertEquals("L2 unless chosen otherwise", zl, settings.cursorButton)
        assertEquals(100, settings.cursorSpeed)

        // Another button, and a faster cursor.
        PreferenceManager.getDefaultSharedPreferences(activity).edit()
            .putString(ReaderSettings.KEY_CURSOR_BUTTON, buttonR.toString())
            .putInt(ReaderSettings.KEY_CURSOR_SPEED, 200).commit()
        HoverCursor.configure(settings.cursorButton, settings.cursorSpeed)
        assertFalse("L2 is the game's again", hold())
        assertTrue(HoverCursor.onButton(buttonR, true, HoverCursor.SOURCE_AXIS))
        val x = HoverCursor.x
        drag(1500f, 300f, 1550f, 300f)
        assertEquals(x + 50f * 2f * 1920f / bottomArea.width, HoverCursor.x, 1f)

        // Held from two places (a key and the trigger's axis): one letting go is not enough.
        HoverCursor.onButton(buttonR, true, HoverCursor.SOURCE_KEY)
        HoverCursor.onButton(buttonR, false, HoverCursor.SOURCE_AXIS)
        assertTrue(HoverCursor.active)
        HoverCursor.onButton(buttonR, false, HoverCursor.SOURCE_KEY)
        assertFalse(HoverCursor.active)

        // The emulator's own hotkey binding holds it as well.
        HoverCursor.onHotkey(true)
        assertTrue(HoverCursor.active)
        HoverCursor.onHotkey(false)
        assertFalse(HoverCursor.active)

        // "None": no button holds the cursor.
        HoverCursor.configure(HoverCursor.BUTTON_NONE, 100)
        assertFalse(HoverCursor.onButton(HoverCursor.BUTTON_NONE, true, HoverCursor.SOURCE_KEY))
        assertFalse(HoverCursor.onButton(buttonR, true, HoverCursor.SOURCE_KEY))

        // No game with a text profile is running: the button is the game's, pressed or not.
        HoverCursor.configure(zl, 100)
        HoverCursor.armed = { false }
        assertFalse(hold())
        assertFalse(HoverCursor.active)
        HoverCursor.onHotkey(true)
        assertFalse(HoverCursor.active)
        PreferenceManager.getDefaultSharedPreferences(activity).edit().clear().commit()
        assertNotNull(overlay)
    }
}
