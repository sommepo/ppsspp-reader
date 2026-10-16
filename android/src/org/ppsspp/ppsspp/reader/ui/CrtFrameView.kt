// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.View
import java.util.concurrent.Executors

/**
 * The tube's frame around the game's picture, with the CRT picture on and a frame chosen
 * (its settings' "Frame", "Room" and "Wear"): the moulded case, the recess, the seal and the
 * recess's shadow on the glass, as the computer's reader window draws it (reader/core/
 * crt_frame.h, the same drawing). Made in the background whenever the view's size, the game's
 * picture or the CRT settings change, and drawn under everything else of the reader's layer.
 * The same file in every reader fork (the Flycast Reader fork's tools/reader/crt.py copies it).
 */
class CrtFrameView(
    context: Context,
    /** Where the game's picture is, in this view's coordinates; null while there is none. */
    private val picture: () -> RectF?
) : View(context) {
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "CrtFrame").apply { isDaemon = true } }
    private var bitmap: Bitmap? = null
    private var made: String? = null
    private var asked = 0

    private val watch = object : Runnable {
        override fun run() {
            update()
            main.postDelayed(this, WATCH_MS)
        }
    }

    init {
        // Never takes a touch: the game and the reader's layer have them.
        isClickable = false
        isFocusable = false
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        main.post(watch)
    }

    override fun onDetachedFromWindow() {
        main.removeCallbacks(watch)
        super.onDetachedFromWindow()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        update()
    }

    /** Asks for a new frame when anything it is made from changed. Cheap otherwise. */
    fun update() {
        val area = picture()
        val revision = try {
            CrtNative.revision()
        } catch (_: UnsatisfiedLinkError) {
            return // no native side (tests)
        }
        val w = width
        val h = height
        val key = if (area == null || w <= 0 || h <= 0) {
            "none"
        } else {
            "$w,$h,${area.left.toInt()},${area.top.toInt()},${area.width().toInt()},${area.height().toInt()},$revision"
        }
        if (key == made) return
        made = key
        val ticket = ++asked
        if (area == null || w <= 0 || h <= 0) {
            show(null, ticket)
            return
        }
        worker.execute {
            val colours = try {
                CrtNative.frame(w, h, area.left, area.top, area.width(), area.height())
            } catch (_: UnsatisfiedLinkError) {
                null
            }
            val made = colours?.let { Bitmap.createBitmap(it, w, h, Bitmap.Config.ARGB_8888) }
            main.post { show(made, ticket) }
        }
    }

    private fun show(made: Bitmap?, ticket: Int) {
        if (ticket != asked) return // a newer one was asked for meanwhile
        bitmap = made
        invalidate()
    }

    /** Whether a frame is drawn now (for tests). */
    val showing: Boolean get() = bitmap != null

    override fun onDraw(canvas: Canvas) {
        bitmap?.let { canvas.drawBitmap(it, 0f, 0f, null) }
    }

    private companion object {
        /** How often the picture's place and the settings are looked at again. */
        const val WATCH_MS = 500L
    }
}
