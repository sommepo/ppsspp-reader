// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.content.Context
import android.graphics.RectF
import android.provider.Settings
import android.view.View
import android.view.animation.AnimationUtils
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.exp

/**
 * How the mark on the word looked up goes from one word to the next: it glides there.
 *
 * Each edge of each of the mark's rectangles (one a row) follows where it should be on a
 * critically damped spring, so a mark sent somewhere else halfway keeps its speed and turns,
 * and settles without a bounce. The edge in front is a little quicker than the one behind, so
 * the mark stretches very slightly on its way and draws itself together as it arrives. A mark
 * that appears grows in from just smaller and fades in; one taken away fades out where it was.
 * With the system's animations switched off it simply is where it should be.
 *
 * The same numbers as reader_mark_glide.h in the desktop app. [view] is redrawn while it moves:
 * call [frame] at the start of its onDraw, then draw [drawn] at [opacity].
 */
class MarkGlide(private val view: View) {

    private class Edge(var at: Float, var target: Float, val closeEnough: Float = 0.2f,
                       val slowEnough: Float = 6f) {
        var speed = 0f
        var quickness = 1f

        /** One step of a critically damped spring, exactly (no step size to go wrong). */
        fun advance(omega: Float, dt: Float) {
            val x = at - target
            val decay = exp(-omega * dt)
            val k = (speed + omega * x) * dt
            at = target + (x + k) * decay
            speed = (speed - omega * k) * decay
            if (resting) {
                at = target
                speed = 0f
            }
        }

        val resting get() = abs(at - target) < closeEnough && abs(speed) < slowEnough
    }

    private class Box(val left: Edge, val top: Edge, val right: Edge, val bottom: Edge) {
        var leaving = false
        val edges get() = arrayOf(left, top, right, bottom)
        val resting get() = edges.all { it.resting }
        fun copy() = Box(Edge(left.at, left.target), Edge(top.at, top.target),
            Edge(right.at, right.target), Edge(bottom.at, bottom.target))
    }

    private val boxes = mutableListOf<Box>()
    private var fade = fadeEdge(1f, 1f)
    private var lastFrame = 0L

    /** True while anything of the mark is to be drawn, a fading one too. */
    val visible get() = boxes.isNotEmpty()

    val moving get() = boxes.isNotEmpty() && (!fade.resting || boxes.any { !it.resting })

    val opacity get() = fade.at.coerceIn(0f, 1f)

    /** Where the mark is drawn now. */
    val drawn: List<RectF>
        get() = boxes.map {
            RectF(minOf(it.left.at, it.right.at), minOf(it.top.at, it.bottom.at),
                maxOf(it.left.at, it.right.at), maxOf(it.top.at, it.bottom.at))
        }

    /**
     * The mark is to be at [to] (empty: nowhere). [glide]: from where it is drawn now, else at
     * once (the picture moved under it). A mark that was nowhere grows in either way.
     */
    fun setTarget(to: List<RectF>, glide: Boolean) {
        val wasMoving = moving
        if (!animationsOn(view.context)) {
            boxes.clear()
            to.forEach { boxes.add(Box(Edge(it.left, it.left), Edge(it.top, it.top),
                Edge(it.right, it.right), Edge(it.bottom, it.bottom))) }
            fade = fadeEdge(1f, 1f)
            view.invalidate()
            return
        }
        when {
            to.isEmpty() -> fade.target = 0f
            boxes.isEmpty() -> {
                for (rect in to) {
                    val dx = rect.width() * (1 - APPEAR_SCALE) / 2
                    val dy = rect.height() * (1 - APPEAR_SCALE) / 2
                    boxes.add(Box(Edge(rect.left + dx, rect.left), Edge(rect.top + dy, rect.top),
                        Edge(rect.right - dx, rect.right), Edge(rect.bottom - dy, rect.bottom)))
                }
                fade = fadeEdge(0f, 1f)
            }
            !glide -> {
                fade.target = 1f
                boxes.clear()
                to.forEach { boxes.add(Box(Edge(it.left, it.left), Edge(it.top, it.top),
                    Edge(it.right, it.right), Edge(it.bottom, it.bottom))) }
            }
            else -> {
                fade.target = 1f
                val from = bounds(drawn)
                val onto = bounds(to)
                // A row more than before starts from the last row drawn; a row fewer joins the
                // last one.
                while (boxes.size < to.size) boxes.add(boxes.last().copy())
                boxes.forEachIndexed { i, box ->
                    val rect = to[minOf(i, to.size - 1)]
                    box.leaving = i >= to.size
                    box.left.target = rect.left
                    box.top.target = rect.top
                    box.right.target = rect.right
                    box.bottom.target = rect.bottom
                }
                val dx = onto.centerX() - from.centerX()
                val dy = onto.centerY() - from.centerY()
                for (box in boxes) {
                    box.left.quickness = quickness(-dx)
                    box.right.quickness = quickness(dx)
                    box.top.quickness = quickness(-dy)
                    box.bottom.quickness = quickness(dy)
                }
            }
        }
        if (!wasMoving) lastFrame = AnimationUtils.currentAnimationTimeMillis()
        view.invalidate()
    }

    /** Moves the mark on to this frame; asks for the next one while it is still moving. */
    fun frame() {
        val now = AnimationUtils.currentAnimationTimeMillis()
        if (!moving) {
            lastFrame = now
            return
        }
        val dt = ((now - lastFrame) / 1000f).coerceIn(0f, 0.05f)
        lastFrame = now
        for (box in boxes) {
            for (edge in box.edges) edge.advance(OMEGA * edge.quickness, dt)
        }
        fade.advance(FADE_OMEGA, dt)
        boxes.removeAll { it.leaving && it.resting }
        if (fade.target == 0f && fade.resting) boxes.clear()
        if (moving) view.postInvalidateOnAnimation()
    }

    /** Gone at once, without a fade (the text it was on is gone). */
    fun clear() {
        boxes.clear()
        fade = fadeEdge(1f, 1f)
    }

    private companion object {
        /** How quickly it all moves: about a fifth of a second to the next word, by eye. */
        const val OMEGA = (2 * PI / 0.20).toFloat()
        const val FADE_OMEGA = (2 * PI / 0.16).toFloat()
        const val LEAD = 1.15f
        const val TRAIL = 0.9f
        const val APPEAR_SCALE = 0.9f

        fun fadeEdge(from: Float, to: Float) = Edge(from, to, closeEnough = 0.002f, slowEnough = 0.06f)

        fun quickness(towards: Float) = when {
            towards > 0.5f -> LEAD
            towards < -0.5f -> TRAIL
            else -> 1f
        }

        fun bounds(rects: List<RectF>): RectF {
            val out = RectF(rects.first())
            rects.forEach { out.union(it) }
            return out
        }

        fun animationsOn(context: Context): Boolean =
            Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) != 0f
    }
}
