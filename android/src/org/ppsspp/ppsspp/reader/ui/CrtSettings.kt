// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.content.Context
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.widget.SwitchCompat
import androidx.preference.PreferenceManager
import org.ppsspp.ppsspp.R
import java.util.Locale
import kotlin.math.roundToInt

/**
 * The CRT picture's bridge to the native side (reader/core/crt_jni.h): the table of what can
 * be set, and the values, which the renderer reads there.
 */
object CrtNative {
    /** A line a row of the table, then a line for every ready-made look. */
    external fun table(): String

    /** Takes values as the kept text: "enabled=1;warp=0.35". What it leaves out stays. */
    external fun apply(text: String)

    /** Every value, as the kept text. */
    external fun values(): String

    /** Sets a ready-made look's values. False when there is no such look. */
    external fun look(key: String): Boolean

    /** The look the values are those of, or "" when they are the user's own. */
    external fun currentLook(): String

    /** A number that changes whenever a value does: the frame is drawn again then. */
    external fun revision(): Int

    /**
     * The tube's frame (reader/core/crt_frame.h) for a screen of [width] by [height] pixels
     * around the picture at [left], [top], [pictureWidth] by [pictureHeight]: colours for
     * Bitmap.createBitmap, or null when no frame is wanted (the CRT picture off, or "None").
     */
    external fun frame(width: Int, height: Int, left: Float, top: Float, pictureWidth: Float, pictureHeight: Float): IntArray?
}

/**
 * The CRT picture on Android: the game shown as a cathode-ray tube would show it. The same
 * file in every reader fork (the Flycast Reader fork's tools/reader/crt.py copies it), and
 * built from the table the native side hands over: nothing here names a slider.
 */
object CrtSettings {
    /** The preference the values are kept in, as one line of text. */
    const val KEY = "reader_crt"

    /** How much the glass bends the picture while the CRT picture is on; 0 otherwise. */
    @Volatile
    private var bend = 0f

    /** Hands the kept values to the renderer. When a game starts, and after a change. */
    fun load(context: Context) {
        val kept = PreferenceManager.getDefaultSharedPreferences(context).getString(KEY, "") ?: ""
        try {
            CrtNative.apply(kept)
            note(CrtNative.values())
        } catch (_: UnsatisfiedLinkError) {
            // Without the native library (tests off the device) there is no picture to change.
            note(kept)
        }
    }

    /** Remembers from the kept text what the layer over the game's text needs to know. */
    internal fun note(values: String) {
        var enabled = false
        var warp = 0f
        for (pair in values.split(';')) {
            val at = pair.indexOf('=')
            if (at <= 0) continue
            val number = pair.substring(at + 1).toFloatOrNull() ?: continue
            when (pair.substring(0, at)) {
                "enabled" -> enabled = number >= 0.5f
                "warp" -> warp = number
            }
        }
        bend = if (enabled) warp.coerceIn(0f, 1f) else 0f
    }

    /**
     * Where on the screen a place of the picture is seen once the glass has bent it: [point]
     * holds x and y, each 0 to 1 from the picture's top left corner, and is changed in place.
     * As reader/core/crt.cpp's ToScreen.
     */
    fun toScreen(point: FloatArray) {
        val warp = bend
        if (warp <= 0f) return
        val x = point[0]
        val y = point[1]
        var sx = x
        var sy = y
        repeat(6) {
            val cx = sx * 2f - 1f
            val cy = sy * 2f - 1f
            val bx = cx * (1f + warp * 0.062f * cy * cy) * 0.5f + 0.5f
            val by = cy * (1f + warp * 0.082f * cx * cx) * 0.5f + 0.5f
            sx += x - bx
            sy += y - by
        }
        point[0] = sx
        point[1] = sy
    }
}

/** The CRT picture's page of the reader's settings: a switch, the looks, a slider for each part. */
class CrtSettingsActivity : ReaderScreen() {
    override val titleRes = R.string.reader_title

    private class Row(
        val key: String,
        val name: String,
        val group: String,
        val hint: String,
        val kind: String,
        val low: Float,
        val high: Float,
        val step: Float,
        val choices: List<Pair<Float, String>>
    )

    private val refreshers = ArrayList<(Map<String, Float>) -> Unit>()
    private var quiet = false
    private var looks: Spinner? = null
    private val lookKeys = ArrayList<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        toolbar.title = TITLE
        val column = scrollingColumn()
        val table = try {
            CrtSettings.load(applicationContext)
            CrtNative.table()
        } catch (_: UnsatisfiedLinkError) {
            column.addView(note("The CRT picture needs the emulator's own library, which is not loaded here."))
            return
        }
        val rows = ArrayList<Row>()
        val lookNames = ArrayList<String>()
        for (line in table.lines()) {
            val fields = line.split('\t')
            if (fields.size == 3 && fields[0] == "#look") {
                lookKeys.add(fields[1])
                lookNames.add(fields[2])
            } else if (fields.size >= 10) {
                rows.add(
                    Row(
                        fields[0], fields[1], fields[2], fields[3], fields[4],
                        fields[5].toFloat(), fields[6].toFloat(), fields[7].toFloat(),
                        fields[9].split('|').filter { it.contains('=') }.map {
                            it.substringBefore('=').toFloat() to it.substringAfter('=')
                        }
                    )
                )
            }
        }

        var group = ""
        for (row in rows) {
            if (row.key != "enabled" && row.group != group) {
                group = row.group
                column.addView(heading(group))
            }
            when (row.kind) {
                "toggle" -> column.addView(toggle(row))
                "choice" -> column.addView(labelled(row, choice(row)))
                else -> column.addView(labelled(row, slider(row)))
            }
            if (row.key == "enabled") {
                // The ready-made looks come straight after the switch.
                lookKeys.add("")
                lookNames.add("My own")
                val spinner = spinner(lookNames) { index ->
                    val key = lookKeys[index]
                    if (key.isNotEmpty() && CrtNative.look(key)) changed()
                }
                looks = spinner
                column.addView(labelled("Look", "Ready-made pictures. Moving a slider makes the picture your own.", spinner))
            }
        }
        refresh()
    }

    override fun onPause() {
        save()
        super.onPause()
    }

    private fun values(): Map<String, Float> = CrtNative.values().split(';').mapNotNull { pair ->
        val at = pair.indexOf('=')
        if (at <= 0) null else pair.substring(at + 1).toFloatOrNull()?.let { pair.substring(0, at) to it }
    }.toMap()

    private fun save() {
        val text = try {
            CrtNative.values()
        } catch (_: UnsatisfiedLinkError) {
            return
        }
        PreferenceManager.getDefaultSharedPreferences(applicationContext).edit().putString(CrtSettings.KEY, text).apply()
        CrtSettings.note(text)
    }

    /** A control was moved: the value is the renderer's already; keep it and show the rest. */
    private fun changed() {
        save()
        refresh()
    }

    private fun refresh() {
        quiet = true
        val now = values()
        for (each in refreshers) each(now)
        looks?.setSelection(lookKeys.indexOf(CrtNative.currentLook()).let { if (it < 0) lookKeys.size - 1 else it })
        quiet = false
    }

    private fun set(key: String, value: Float) {
        if (quiet) return
        CrtNative.apply(key + "=" + String.format(Locale.ROOT, "%.3f", value))
        changed()
    }

    private fun toggle(row: Row): View = SwitchCompat(this).apply {
        text = row.name
        textSize = 16f
        setPadding(0, dp(10f), 0, dp(10f))
        setOnCheckedChangeListener { _, on -> set(row.key, if (on) 1f else 0f) }
        refreshers.add { now -> isChecked = (now[row.key] ?: 0f) >= 0.5f }
    }

    private fun slider(row: Row): View {
        val steps = ((row.high - row.low) / row.step).roundToInt()
        val number = TextView(this).apply {
            textSize = 14f
            gravity = Gravity.END
            minWidth = dp(44f)
        }
        val bar = SeekBar(this).apply {
            max = steps
            keyProgressIncrement = 1
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                    val value = row.low + row.step * progress
                    number.text = String.format(Locale.ROOT, "%.2f", value)
                    if (fromUser) set(row.key, value)
                }

                override fun onStartTrackingTouch(bar: SeekBar) = Unit
                override fun onStopTrackingTouch(bar: SeekBar) = Unit
            })
        }
        refreshers.add { now ->
            bar.progress = (((now[row.key] ?: row.low) - row.low) / row.step).roundToInt()
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(bar, LinearLayout.LayoutParams(0, WRAP, 1f))
            addView(number, LinearLayout.LayoutParams(WRAP, WRAP))
        }
    }

    private fun choice(row: Row): View {
        val spinner = spinner(row.choices.map { it.second }) { index -> set(row.key, row.choices[index].first) }
        refreshers.add { now ->
            val value = now[row.key] ?: row.low
            val index = row.choices.indexOfFirst { kotlin.math.abs(it.first - value) < 0.001f }
            if (index >= 0) spinner.setSelection(index)
        }
        return spinner
    }

    private fun spinner(names: List<String>, chosen: (Int) -> Unit) = Spinner(this).apply {
        adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, names)
        onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (!quiet) chosen(position)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }

    private fun labelled(row: Row, control: View): View = labelled(row.name, row.hint, control)

    private fun labelled(name: String, hint: String, control: View): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(8f), 0, dp(4f))
        addView(body(name, 16f))
        if (hint.isNotEmpty()) addView(note(hint))
        addView(control, LinearLayout.LayoutParams(MATCH, WRAP))
    }

    private companion object {
        const val TITLE = "CRT picture"
    }
}
