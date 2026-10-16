// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import android.graphics.Color
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.ppsspp.ppsspp.reader.ui.TouchButtons
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** The buttons a game played by touch still needs: the script's line, and the column. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class TouchButtonsTest {
    private val context: Context = ApplicationProvider.getApplicationContext()

    @Test
    fun theScriptSaysWhichButtons() {
        val info = ReaderCatalog.describe(
            "ULJM99999",
            "-- reader: title 作り話\n-- reader: touch press\n-- reader: buttons Up DOWN circle\n",
            own = false
        )
        assertEquals(listOf("up", "down", "circle"), info.buttons)
        assertEquals(emptyList<String>(), ReaderCatalog.describe("ULJM99999", "-- reader: touch press\n", false).buttons)
        // The two bundled games whose choices a tap cannot pick.
        ReaderCatalog.forget()
        assertEquals(listOf("up", "down", "left", "right", "circle", "cross", "square", "triangle"), ReaderCatalog.buttons(context, "ULJM05887"))
        assertEquals(listOf("up", "down", "circle", "cross", "square"), ReaderCatalog.buttons(context, "ULJM05821"))
        assertEquals(emptyList<String>(), ReaderCatalog.buttons(context, "ULJS00219"))
    }

    @Test
    fun aHeldButtonIsHeldInTheGame() {
        val pressed = ArrayList<Pair<Int, Boolean>>()
        val buttons = TouchButtons(context) { code, down -> pressed += code to down }
        buttons.show(listOf("up", "down", "circle", "nonsense", "triangle"))
        assertEquals(listOf("up", "down", "circle", "triangle"), buttons.names)
        assertEquals(View.VISIBLE, buttons.visibility)

        val root = FrameLayout(context).apply { setBackgroundColor(Color.rgb(40, 70, 110)) }
        root.addView(
            buttons,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.END or android.view.Gravity.CENTER_VERTICAL
            )
        )
        TestSupport.snapshot(root, 1280, 720, "touch-buttons")

        val triangle = buttons.findViewWithTag<View>("triangle")
        val now = SystemClock.uptimeMillis()
        triangle.dispatchTouchEvent(MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, 10f, 10f, 0))
        assertEquals(listOf(12 to true), pressed) // △ is down while the finger stays
        triangle.dispatchTouchEvent(MotionEvent.obtain(now, now + 600, MotionEvent.ACTION_UP, 10f, 10f, 0))
        assertEquals(listOf(12 to true, 12 to false), pressed)

        buttons.show(emptyList())
        assertEquals(View.GONE, buttons.visibility)
        assertTrue(buttons.names.isEmpty())
    }

    @Test
    fun everyButtonAndATabThatTucksThemAwayForGood() {
        androidx.preference.PreferenceManager.getDefaultSharedPreferences(context).edit()
            .remove(TouchButtons.KEY_TUCKED).commit()
        val buttons = TouchButtons(context) { _, _ -> }
        buttons.show(TouchButtons.ALL)
        assertEquals(TouchButtons.ALL, buttons.names)
        assertTrue(buttons.buttonsShown)
        val root = FrameLayout(context).apply { setBackgroundColor(Color.rgb(40, 70, 110)) }
        root.addView(
            buttons,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.END or android.view.Gravity.CENTER_VERTICAL
            )
        )
        TestSupport.snapshot(root, 1280, 720, "touch-buttons-all")

        buttons.findViewWithTag<View>(TouchButtons.TAB).performClick()
        assertTrue(!buttons.buttonsShown)
        assertEquals(View.VISIBLE, buttons.visibility) // the tab stays, to bring them back
        TestSupport.snapshot(root, 1280, 720, "touch-buttons-tucked")
        // Kept: the next game starts with them tucked away.
        val next = TouchButtons(context) { _, _ -> }
        next.show(TouchButtons.ALL)
        assertTrue(!next.buttonsShown)
        next.findViewWithTag<View>(TouchButtons.TAB).performClick()
        assertTrue(next.buttonsShown)
        assertTrue(!TouchButtons(context) { _, _ -> }.tucked)
    }
}
