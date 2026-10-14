// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.Toast
import java.io.File
import java.util.IdentityHashMap
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.dict.DictionaryStore
import org.ppsspp.ppsspp.reader.dict.LookupResult
import org.ppsspp.ppsspp.reader.mining.ClipPlayer
import org.ppsspp.ppsspp.reader.mining.Miner
import org.ppsspp.ppsspp.reader.screen.Area

/**
 * Dictionary lookups over some text: runs the lookup for a tapped character, shows the popup
 * beside the text, marks the matched word and makes cards from the results.
 *
 * It fills its parent. While the popup is closed nothing here takes touches; while it is open a
 * transparent scrim catches taps outside the popup, so closing never clicks what lies beneath.
 *
 * It also puts the question a card may have before it is made (does it get its line's audio?),
 * for the cards made from the popup and for those made elsewhere on the same screen.
 */
@SuppressLint("ViewConstructor")
class LookupLayer(context: Context, private val onOpenDictionaries: () -> Unit) :
    FrameLayout(context) {

    /** What shows the text a lookup was made in, and can mark the word that was found. */
    interface Target {
        fun setHighlight(range: IntRange?)
    }

    /**
     * Where the popup belongs, in this layer's coordinates: above or below [keepClear] (the
     * text the word is in, which the popup must not cover), starting near [nearX] (the word).
     */
    class Anchor(val keepClear: Area, val nearX: Float)

    /** The line and position the open popup belongs to. Cards are made from these even when the
     * game has moved on since. */
    private class Pinned(val presented: PresentedLine, val offset: Int, val target: Target)

    /**
     * A view beneath this layer that keeps taking touches while the popup is open: the text the
     * popup belongs to, so another word can be looked up with one tap.
     */
    var passThrough: View? = null

    /** The same for places that are not a view: the game's own text boxes. */
    var passThroughAt: ((x: Float, y: Float) -> Boolean)? = null

    /** Called when the popup goes away, however that came about. */
    var onClosed: (() -> Unit)? = null

    @SuppressLint("ClickableViewAccessibility")
    private val scrim = object : View(context) {
        override fun onTouchEvent(event: MotionEvent): Boolean {
            val through = passThrough
            if (through != null && through.visibility == VISIBLE &&
                event.x >= through.left && event.x < through.right &&
                event.y >= through.top && event.y < through.bottom
            ) {
                return false
            }
            if (passThroughAt?.invoke(event.x, event.y) == true) return false
            if (event.actionMasked == MotionEvent.ACTION_UP) close()
            return true
        }
    }
    private val popup = LookupPopupView(context)
    private val miner = Miner(context)
    private val settings = ReaderSettings(context)
    private var pinned: Pinned? = null

    /** The word the open popup is about: its line and where in the line it is. */
    private var wordLine: PresentedLine? = null
    private var wordRange: IntRange = IntRange.EMPTY

    /**
     * Asked when the popup's time is up: true keeps it open for another while (the cursor is
     * resting on it or on its word).
     */
    var holdOpen: (() -> Boolean)? = null

    /** The popup goes away by itself when it has been left alone for the time in the settings. */
    private val autoHide = Runnable {
        if (!isOpen) return@Runnable
        val held = holdOpen?.invoke() == true
        if (held) cursorInside = true
        if (isAsking || held) keepOpen() else close()
    }

    /** The cursor left the word and the popup and did not come back in time. */
    private val exitHide = Runnable {
        exitPending = false
        if (isOpen && !isAsking) close()
    }
    private var exitPending = false
    private var cursorInside = false

    /** Somebody is using the popup: its time starts over. */
    fun keepOpen() {
        removeCallbacks(autoHide)
        val seconds = settings.popupAutoHideSeconds
        if (isOpen && seconds > 0) postDelayed(autoHide, seconds * 1000L)
    }

    /** True when the open popup is about the character at [offset] of [presented]. */
    fun isAbout(presented: PresentedLine, offset: Int): Boolean =
        isOpen && wordLine?.backlogId == presented.backlogId && offset in wordRange

    /**
     * The cursor moved. [inside]: it is on the popup or on the popup's word. Outside, the
     * popup goes away after a moment (long enough to cross from the word to the popup) when
     * the settings say so.
     */
    fun cursorMoved(inside: Boolean) {
        if (!isOpen) return
        if (inside) {
            cursorInside = true
            removeCallbacks(exitHide)
            exitPending = false
            keepOpen()
            return
        }
        if (cursorInside) {
            // Just left: its time counts from now, not from when it was last renewed.
            cursorInside = false
            keepOpen()
        }
        if (settings.popupHideOnCursorExit && !exitPending && !isAsking) {
            exitPending = true
            postDelayed(exitHide, EXIT_GRACE_MS)
        }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        // A finger on the popup (reading on, scrolling) is somebody using it.
        if (isOpen && popupContains(event.x, event.y)) keepOpen()
        return super.dispatchTouchEvent(event)
    }

    /** Counts what the popup has shown: answers about an earlier content are dropped. */
    private var token = 0

    /** Counts lookups: only the latest one's result is shown. */
    private var asked = 0
    private var anchor: Anchor? = null

    /** The notes Anki has for the words of the open popup, as far as it has answered. */
    private val notes = IdentityHashMap<LookupResult, List<Long>>()

    /** A card that waits to hear whether it gets its line's audio. */
    private class Asked(val clip: File, val answer: (keep: Boolean?) -> Unit)

    /** The cards that wait, the one the question is shown for first. */
    private val asking = ArrayDeque<Asked>()
    private val player = ClipPlayer()
    private val prompt = AudioPromptView(context)

    /** While the question is up, a tap beside it calls the card off and reaches nothing else. */
    @SuppressLint("ClickableViewAccessibility")
    private val promptScrim = object : View(context) {
        override fun onTouchEvent(event: MotionEvent): Boolean {
            if (event.actionMasked == MotionEvent.ACTION_UP) answer(null)
            return true
        }
    }

    /** For cards made on this screen: asks here whether a card gets its line's audio. */
    val audioQuestion = Miner.AudioQuestion { clip, answer ->
        asking.addLast(Asked(clip, answer))
        if (asking.size == 1) showQuestion()
    }

    /** True while a card's question about its audio is up. */
    val isAsking: Boolean get() = asking.isNotEmpty()

    val isOpen: Boolean get() = popup.visibility == VISIBLE

    /** True where the open popup is, in this layer's coordinates. */
    fun popupContains(x: Float, y: Float): Boolean =
        isOpen && x >= popup.left && x < popup.right && y >= popup.top && y < popup.bottom

    init {
        layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT)
        scrim.visibility = GONE
        addView(scrim, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))

        popup.visibility = GONE
        popup.onOpenDictionaries = {
            close()
            onOpenDictionaries()
        }
        popup.onAdd = { result -> mine(result) }
        popup.onShowNotes = { result -> showNotes(result) }
        // What a box offers can change after the popup was placed, and its size with it.
        popup.addOnLayoutChangeListener { _, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom ->
            if (isOpen && (right - left != oldRight - oldLeft || bottom - top != oldBottom - oldTop)) {
                post { if (isOpen) position() }
            }
        }
        addView(popup, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))

        promptScrim.visibility = GONE
        addView(promptScrim, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        prompt.visibility = GONE
        prompt.onListen = { listen() }
        prompt.onKeep = { answer(true) }
        prompt.onDrop = { answer(false) }
        addView(prompt, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
    }

    override fun onDetachedFromWindow() {
        // Nobody is left to answer: the cards that wait are not made.
        while (asking.isNotEmpty()) answer(null)
        removeCallbacks(autoHide)
        removeCallbacks(exitHide)
        super.onDetachedFromWindow()
    }

    /**
     * Puts the popup beside its anchor: above the text if it fits there, else below, else on
     * the side with more room and only as tall as that room. It is only as large as its content.
     */
    private fun position() {
        val anchor = anchor ?: return
        if (width == 0 || height == 0) return
        val margin = dp(6f)
        val widest = (width - 2 * margin).coerceAtMost(dp(MAX_WIDTH_DP)).coerceAtLeast(dp(120f))
        val above = (anchor.keepClear.top - 2 * margin).toInt()
        val below = (height - anchor.keepClear.bottom - 2 * margin).toInt()
        fun measure(tallest: Int) {
            popup.maxWidth = widest
            popup.maxHeight = tallest.coerceAtLeast(dp(56f))
            popup.measure(
                MeasureSpec.makeMeasureSpec(widest, MeasureSpec.AT_MOST),
                MeasureSpec.makeMeasureSpec(popup.maxHeight, MeasureSpec.AT_MOST)
            )
        }
        measure(minOf(height * settings.popupHeightPercent / 100, maxOf(above, below)))
        val goesAbove = popup.measuredHeight <= above || above >= below
        val room = if (goesAbove) above else below
        if (popup.measuredHeight > room) measure(room)

        val params = popup.layoutParams as LayoutParams
        params.gravity = Gravity.TOP or Gravity.START
        params.width = LayoutParams.WRAP_CONTENT
        params.height = LayoutParams.WRAP_CONTENT
        params.leftMargin = (anchor.nearX.toInt() - dp(10f))
            .coerceIn(margin, maxOf(margin, width - margin - popup.measuredWidth))
        params.topMargin = if (goesAbove) {
            (anchor.keepClear.top.toInt() - margin - popup.measuredHeight).coerceAtLeast(margin)
        } else {
            (anchor.keepClear.bottom.toInt() + margin).coerceAtMost(maxOf(margin, height - margin - popup.measuredHeight))
        }
        params.rightMargin = 0
        params.bottomMargin = 0
        popup.layoutParams = params
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        post {
            position()
            placeQuestion()
        }
    }

    // ---- The question about a card's audio ----

    private fun showQuestion() {
        prompt.prepare(settings.popupScale)
        promptScrim.visibility = VISIBLE
        prompt.visibility = VISIBLE
        placeQuestion()
    }

    /**
     * Beside the popup the card was asked from, on the side away from the text the popup
     * belongs to, with its right edge at the popup's; over the popup's own edge where there is
     * no room beside it. In the middle when there is no popup.
     */
    private fun placeQuestion() {
        if (prompt.visibility != VISIBLE || width == 0 || height == 0) return
        val margin = dp(6f)
        prompt.measure(
            MeasureSpec.makeMeasureSpec((width - 2 * margin).coerceAtLeast(0), MeasureSpec.AT_MOST),
            MeasureSpec.makeMeasureSpec((height - 2 * margin).coerceAtLeast(0), MeasureSpec.AT_MOST)
        )
        val wide = prompt.measuredWidth
        val tall = prompt.measuredHeight
        var left = (width - wide) / 2
        var top = (height - tall) / 2
        val text = anchor?.keepClear
        if (isOpen && popup.width > 0) {
            left = popup.right - wide
            top = if (text == null || popup.top < text.top) {
                (popup.top - margin - tall).takeIf { it >= margin } ?: popup.top
            } else {
                (popup.bottom + margin).takeIf { it + tall <= height - margin } ?: (popup.bottom - tall)
            }
        }
        val params = prompt.layoutParams as LayoutParams
        params.gravity = Gravity.TOP or Gravity.START
        params.leftMargin = left.coerceIn(margin, maxOf(margin, width - margin - wide))
        params.topMargin = top.coerceIn(margin, maxOf(margin, height - margin - tall))
        params.rightMargin = 0
        params.bottomMargin = 0
        prompt.layoutParams = params
    }

    /** Plays the clip the question is about, or stops it. */
    private fun listen() {
        val asked = asking.firstOrNull() ?: return
        if (player.isPlaying) {
            player.stop()
            prompt.setPlaying(false)
        } else {
            prompt.setPlaying(true)
            player.play(asked.clip) { prompt.setPlaying(false) }
        }
    }

    /** Answers the question that is up: with the audio, without it, or (null) no card. */
    private fun answer(keep: Boolean?) {
        val asked = asking.removeFirstOrNull() ?: return
        player.stop()
        if (asking.isEmpty()) {
            prompt.visibility = GONE
            promptScrim.visibility = GONE
        } else {
            showQuestion()
        }
        asked.answer(keep)
    }

    // ---- Lookups ----

    /**
     * Looks up the word starting at [offset] of [presented]'s text, shown in [target], and
     * shows what was found at [beside]. A [quiet] lookup that finds no word changes nothing:
     * for lookups nobody asked for by hand, such as under a cursor passing over punctuation.
     */
    fun lookup(target: Target, presented: PresentedLine, offset: Int, beside: Anchor, quiet: Boolean = false) {
        val text = presented.line.text
        if (offset !in text.indices) return
        ReaderSession.noteInteraction()
        val request = ++asked
        val rest = text.substring(offset)
        val store = DictionaryStore.get(context)
        worker.execute {
            val hasDictionaries = store.database.hasEnabledDictionaries()
            val results = if (hasDictionaries) store.lookup.lookup(rest) else emptyList()
            post {
                if (request != asked) return@post
                if (quiet && hasDictionaries && results.isEmpty()) return@post
                val shown = ++token
                anchor = beside
                pinned?.target?.setHighlight(null)
                pinned = null
                wordLine = presented
                wordRange = offset..offset
                notes.clear()
                popup.textScale = settings.popupScale
                when {
                    !hasDictionaries ->
                        popup.showMessage(context.getString(R.string.reader_lookup_no_dictionaries), true)

                    results.isEmpty() -> popup.showMessage(
                        context.getString(R.string.reader_lookup_no_entry, rest.take(8)),
                        false
                    )

                    else -> {
                        wordRange = offset until offset + results[0].matchedText.length
                        target.setHighlight(wordRange)
                        pinned = Pinned(presented, offset, target)
                        popup.allowDuplicates = settings.ankiAllowDuplicates
                        popup.showResults(results)
                        askAnki(shown, presented, offset, results)
                    }
                }
                scrim.visibility = VISIBLE
                popup.visibility = VISIBLE
                position()
                removeCallbacks(exitHide)
                exitPending = false
                // A cursor that was resting on the word when its popup came up is on it.
                cursorInside = holdOpen?.invoke() == true
                keepOpen()
            }
        }
    }

    fun close() {
        token++
        asked++
        pinned?.target?.setHighlight(null)
        pinned = null
        wordLine = null
        wordRange = IntRange.EMPTY
        removeCallbacks(autoHide)
        removeCallbacks(exitHide)
        exitPending = false
        cursorInside = false
        notes.clear()
        val wasOpen = isOpen
        popup.visibility = GONE
        scrim.visibility = GONE
        if (wasOpen) onClosed?.invoke()
    }

    /** Finds out which of [results] Anki has already; their boxes turn into open books. */
    private fun askAnki(request: Int, presented: PresentedLine, offset: Int, results: List<LookupResult>) {
        for (result in results) {
            val range = offset until offset + result.matchedText.length
            miner.findExisting(presented, result, range) { found ->
                // The popup may show other words by now; those were asked about separately.
                if (request != token || found.isNullOrEmpty()) return@findExisting
                notes[result] = found
                if (popup.cardState(result) == LookupPopupView.CardState.ADD) {
                    popup.setCardState(result, LookupPopupView.CardState.IN_DECK)
                }
            }
        }
    }

    private fun mine(result: LookupResult) {
        val pin = pinned ?: return
        val request = token
        val range = pin.offset until pin.offset + result.matchedText.length
        val before = popup.cardState(result) ?: return
        if (before == LookupPopupView.CardState.WORKING) return
        popup.setCardState(result, LookupPopupView.CardState.WORKING)
        miner.mine(pin.presented, result, range, audioQuestion) { outcome ->
            report(context, outcome)
            if (request != token) return@mine
            val known = when (outcome) {
                is Miner.Outcome.Added -> notes[result].orEmpty() + outcome.noteId
                is Miner.Outcome.Exists -> outcome.notes
                is Miner.Outcome.Failed, Miner.Outcome.Cancelled -> null
            }
            if (known != null) notes[result] = known
            popup.setCardState(result, if (known != null) LookupPopupView.CardState.IN_DECK else before)
        }
    }

    private fun showNotes(result: LookupResult) {
        val known = notes[result]?.takeIf { it.isNotEmpty() } ?: return
        miner.showNotes(known) { failure ->
            if (failure != null) {
                val message = context.getString(R.string.reader_card_show_failed, failure)
                Toast.makeText(context, message, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun dp(value: Float): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics).toInt()

    companion object {
        /** How long the cursor may be off the word and the popup before the popup goes. */
        const val EXIT_GRACE_MS = 450L
        /** The popup is at most this wide; how tall it may get is a setting. */
        private const val MAX_WIDTH_DP = 340f

        private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderLookup") }

        /** The rectangle of [view] in [layer]'s coordinates. */
        fun areaOf(view: View, layer: View): Area {
            val own = IntArray(2).also { layer.getLocationInWindow(it) }
            val other = IntArray(2).also { view.getLocationInWindow(it) }
            val left = (other[0] - own[0]).toFloat()
            val top = (other[1] - own[1]).toFloat()
            return Area(left, top, left + view.width, top + view.height)
        }

        /** Tells the user how a card request ended. One they called off themselves needs no telling. */
        fun report(context: Context, outcome: Miner.Outcome) {
            val message = when (outcome) {
                is Miner.Outcome.Added -> context.getString(R.string.reader_card_added)
                is Miner.Outcome.Exists -> context.getString(R.string.reader_card_exists)
                is Miner.Outcome.Failed -> context.getString(R.string.reader_card_failed, outcome.message)
                Miner.Outcome.Cancelled -> return
            }
            Toast.makeText(context, message, Toast.LENGTH_LONG).show()
        }
    }
}
