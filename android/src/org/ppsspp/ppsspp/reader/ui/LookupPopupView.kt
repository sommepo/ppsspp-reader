// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.InsetDrawable
import android.graphics.drawable.StateListDrawable
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.ReplacementSpan
import android.text.style.StyleSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.IdentityHashMap
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.dict.GlossaryRenderer
import org.ppsspp.ppsspp.reader.dict.LookupResult
import org.ppsspp.ppsspp.reader.dict.TermEntry

/**
 * The dictionary popup, kept as small as it can be: it is only as wide and as tall as what it
 * says. Per headword there is one line with the word, its reading and how it was inflected,
 * then one line per sense with its glosses run together. In a corner of each headword's part
 * (the bottom left unless the settings choose another, [cardCorner]) sits a boxed button: a
 * plus to make a card, or an open book when Anki has the word already. Where a headword's part
 * runs on past what the popup shows, its box waits at the popup's own edge on that side, so it
 * can be pressed without scrolling.
 */
@SuppressLint("ViewConstructor")
class LookupPopupView(context: Context) : FrameLayout(context) {
    /** What the box at the foot of a headword offers. */
    enum class CardState {
        /** Not in Anki, as far as is known: a plus that makes a card. */
        ADD,

        /** A card is being made. */
        WORKING,

        /** Anki has the word: an open book that shows its notes. */
        IN_DECK
    }

    var onAdd: ((result: LookupResult) -> Unit)? = null
    var onShowNotes: ((result: LookupResult) -> Unit)? = null
    var onOpenDictionaries: (() -> Unit)? = null

    /** Keep offering the plus beside the book for a word Anki already has. */
    var allowDuplicates = false

    /** The corner of a headword's part where its boxes stand. */
    enum class Corner(val top: Boolean, val left: Boolean) {
        TOP_LEFT(true, true),
        TOP_RIGHT(true, false),
        BOTTOM_LEFT(false, true),
        BOTTOM_RIGHT(false, false);

        companion object {
            /** From the setting's value ("bottom_left" and so on); the bottom left otherwise. */
            fun of(value: String?): Corner = values().firstOrNull { it.name.equals(value, ignoreCase = true) } ?: BOTTOM_LEFT
        }
    }

    /** Where the boxes stand: set before [showResults]. */
    var cardCorner = Corner.BOTTOM_LEFT

    /** The popup grows with its content up to these, then wraps and scrolls. */
    var maxWidth: Int = Int.MAX_VALUE
        set(value) {
            if (field != value) {
                field = value
                requestLayout()
            }
        }
    var maxHeight: Int = Int.MAX_VALUE
        set(value) {
            if (field != value) {
                field = value
                requestLayout()
            }
        }

    /** Size of the popup's text relative to its normal size, in percent. */
    var textScale: Int = 100
        set(value) {
            field = value.coerceIn(50, 250)
        }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(limit(widthMeasureSpec, maxWidth), limit(heightMeasureSpec, maxHeight))
    }

    /** At most [max], and never more than the parent allows. */
    private fun limit(spec: Int, max: Int): Int {
        val allowed = if (MeasureSpec.getMode(spec) == MeasureSpec.UNSPECIFIED) {
            Int.MAX_VALUE
        } else {
            MeasureSpec.getSize(spec)
        }
        return MeasureSpec.makeMeasureSpec(minOf(allowed, max).coerceAtMost(1 shl 29), MeasureSpec.AT_MOST)
    }

    private val scroll = ScrollView(context)

    // The lines between headwords are the layout's own dividers: a divider view would have to
    // say how wide it is, and "as wide as the popup" makes the popup as wide as it may be.
    private val list = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        showDividers = LinearLayout.SHOW_DIVIDER_MIDDLE
        dividerDrawable = ConsoleLook.rule(resources.displayMetrics.density)
    }

    /** One headword's part of the popup. */
    private class Entry(
        val result: LookupResult,
        val column: View,
        val headline: TextView,
        val headlineText: CharSequence,
        val last: TextView,
        val lastText: CharSequence,
        val add: ImageButton,
        val book: ImageButton,
        val actions: View
    ) {
        var state = CardState.ADD
        lateinit var view: View
    }

    private val entries = IdentityHashMap<LookupResult, Entry>()

    init {
        background = ConsoleLook.ground(resources.displayMetrics.density)
        elevation = dp(6f)
        isClickable = true // taps inside never fall through to the game
        scroll.setPadding(dp(7f).toInt(), dp(5f).toInt(), dp(5f).toInt(), dp(4f).toInt())
        scroll.isVerticalScrollBarEnabled = true
        scroll.setOnScrollChangeListener { _, _, _, _, _ -> placeBoxes() }
        scroll.addView(list, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        addView(scroll, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        placeBoxes()
    }

    /**
     * Keeps every box in reach: one whose headword runs on below what is shown (above it, for a
     * box in a top corner) is moved to that edge of what is shown, as long as that keeps it
     * within its headword's part.
     */
    private fun placeBoxes() {
        val shownFrom = scroll.scrollY
        val shownTo = scroll.scrollY + scroll.height - scroll.paddingTop - scroll.paddingBottom
        for (entry in entries.values) {
            val most = (entry.view.height - entry.actions.height).coerceAtLeast(0)
            entry.actions.translationY = if (cardCorner.top) {
                (shownFrom - entry.view.top).coerceIn(0, most).toFloat()
            } else {
                -(entry.view.bottom - shownTo).coerceIn(0, most).toFloat()
            }
        }
    }

    /** How far the list is scrolled, for tests and for callers that want to scroll it. */
    val scrolled: Int get() = scroll.scrollY

    fun scrollTo(y: Int) = scroll.scrollTo(0, y)

    fun showResults(results: List<LookupResult>) {
        list.removeAllViews()
        entries.clear()
        scroll.scrollTo(0, 0)
        for (result in results) {
            // As wide as the popup, so that every button box sits at its right edge.
            list.addView(
                resultView(result),
                LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT)
            )
        }
    }

    fun showMessage(message: String, offerDictionaries: Boolean) {
        list.removeAllViews()
        entries.clear()
        list.addView(label(message, SENSE_SP, ReaderStyle.POPUP_TEXT).apply { setPadding(0, 0, dp(4f).toInt(), 0) })
        if (offerDictionaries) {
            list.addView(
                label(context.getString(R.string.reader_lookup_open_dictionaries), SENSE_SP, ReaderStyle.POPUP_ACCENT).apply {
                    setPadding(0, dp(8f).toInt(), dp(4f).toInt(), dp(4f).toInt())
                    setOnClickListener { onOpenDictionaries?.invoke() }
                }
            )
        }
    }

    /** What the box of [result] offers, or null when the popup no longer shows it. */
    fun cardState(result: LookupResult): CardState? = entries[result]?.state

    /** Changes what the box of [result] offers. Does nothing when it is no longer shown. */
    fun setCardState(result: LookupResult, state: CardState) {
        val entry = entries[result] ?: return
        entry.state = state
        apply(entry)
    }

    private fun resultView(result: LookupResult): View {
        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(3f).toInt(), 0, bottomRoom())
        }
        val corner = cardCorner
        val headline = label("", HEAD_SP, ReaderStyle.POPUP_TEXT)
        column.addView(headline, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        val senses = senses(result)
        val last: TextView
        val lastText: CharSequence
        if (senses.isNotEmpty()) {
            headline.text = headline(result)
            last = label("", SENSE_SP, ReaderStyle.POPUP_TEXT).apply { setLineSpacing(0f, 1.08f) }
            lastText = senses
            column.addView(last, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        } else {
            last = headline
            lastText = headline(result)
        }

        val add = box(R.drawable.ic_reader_add, R.string.reader_lookup_add, ReaderStyle.POPUP_SIGN, corner)
        add.setOnClickListener { onAdd?.invoke(result) }
        val book = box(R.drawable.ic_reader_open_book, R.string.reader_lookup_in_deck, ReaderStyle.POPUP_BOOK_SIGN, corner)
        book.setOnClickListener { onShowNotes?.invoke(result) }
        val actions = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        val size = LinearLayout.LayoutParams(boxSize() + slopLeft(), boxSize() + slopTop())
        // The plus is the one nearer the corner, the book beside it.
        for (box in if (corner.left) listOf(add, book) else listOf(book, add)) {
            actions.addView(box, LinearLayout.LayoutParams(size))
        }

        val entry = Entry(result, column, headline, if (senses.isNotEmpty()) headline.text else lastText, last, lastText, add, book, actions)
        entries[result] = entry
        apply(entry)
        entry.view = FrameLayout(context).apply {
            background = ConsoleLook.entry(resources.displayMetrics.density, entries.size == 1, headline)
            addView(column, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
            addView(
                actions,
                LayoutParams(
                    LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT,
                    (if (corner.top) Gravity.TOP else Gravity.BOTTOM) or (if (corner.left) Gravity.START else Gravity.END)
                )
            )
        }
        return entry.view
    }

    /** Shows the boxes [entry]'s state calls for and keeps the text clear of them. */
    private fun apply(entry: Entry) {
        val inDeck = entry.state == CardState.IN_DECK
        val working = entry.state == CardState.WORKING
        entry.book.visibility = if (inDeck) VISIBLE else GONE
        entry.add.visibility = if (inDeck && !allowDuplicates) GONE else VISIBLE
        entry.add.isEnabled = !working
        entry.add.alpha = if (working) 0.4f else 1f
        val boxes = (if (entry.book.visibility == VISIBLE) 1 else 0) + (if (entry.add.visibility == VISIBLE) 1 else 0)
        val room = boxes * (boxSize() + slopLeft())
        val corner = cardCorner
        // The text keeps clear of the boxes. Bottom right: a blank as wide as them ends the
        // text, so the last line stops short of them (or the blank takes a line of its own).
        // Top left: such a blank begins the headline. Top right: the headline stops short of
        // them. Bottom left: they have a row of their own under the text.
        fun withRoom(text: CharSequence, before: Boolean, after: Boolean) = SpannableStringBuilder().apply {
            if (before && room > 0) {
                append("￼", Room(room), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                append(' ')
            }
            append(text)
            if (after && room > 0) {
                append(' ')
                append("￼", Room(room), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        val first = corner == Corner.TOP_LEFT
        val end = corner == Corner.BOTTOM_RIGHT
        if (entry.last === entry.headline) {
            entry.last.text = withRoom(entry.lastText, first, end)
        } else {
            entry.headline.text = withRoom(entry.headlineText, first, false)
            entry.last.text = withRoom(entry.lastText, false, end)
        }
        entry.headline.setPadding(0, 0, if (corner == Corner.TOP_RIGHT) room else 0, 0)
        entry.column.setPadding(
            0, dp(3f).toInt(), 0,
            bottomRoom() + if (corner == Corner.BOTTOM_LEFT && boxes > 0) boxSize() + dp(2f).toInt() else 0
        )
    }

    /** Space at the end of a text for what is drawn over that corner. */
    private class Room(val width: Int) : ReplacementSpan() {
        override fun getSize(paint: Paint, text: CharSequence?, start: Int, end: Int, fm: Paint.FontMetricsInt?) =
            width

        override fun draw(
            canvas: Canvas,
            text: CharSequence?,
            start: Int,
            end: Int,
            x: Float,
            top: Int,
            y: Int,
            bottom: Int,
            paint: Paint
        ) = Unit
    }

    // The box is a little taller than a line of senses. It stands on the bottom edge of its
    // headword's part, and that part's text ends [bottomRoom] above the edge, so the box
    // reaches no higher than the last line does.
    private fun boxSize() = dp(BOX_DP * textScale / 100f).toInt()

    private fun bottomRoom() = dp(BOTTOM_DP * textScale / 100f).toInt()

    // The button takes touches a little beyond the box it shows, towards the middle of the popup.
    private fun slopLeft() = dp(8f).toInt()

    private fun slopTop() = dp(10f).toInt()

    private fun box(icon: Int, description: Int, colour: Int, corner: Corner) = ImageButton(context).apply {
        setImageResource(icon)
        imageTintList = ColorStateList.valueOf(colour)
        scaleType = ImageView.ScaleType.FIT_CENTER
        contentDescription = context.getString(description)
        val left = if (corner.left) 0 else slopLeft()
        val right = if (corner.left) slopLeft() else 0
        val top = if (corner.top) 0 else slopTop()
        val bottom = if (corner.top) slopTop() else 0
        background = InsetDrawable(boxBackground(colour), left, top, right, bottom)
        val inset = (boxSize() * 0.17f).toInt()
        setPadding(left + inset, top + inset, right + inset, bottom + inset)
    }

    // Filled, not see-through: a box that waits in the popup's corner has text passing under it.
    private fun boxBackground(colour: Int): Drawable {
        fun shape(fill: Int) = GradientDrawable().apply {
            setColor(fill)
            cornerRadius = dp(4f)
            setStroke(dp(1f).toInt().coerceAtLeast(1), (colour and 0x00FFFFFF) or 0xA0000000.toInt())
        }
        return StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), shape(ReaderStyle.POPUP_BOX_PRESSED))
            addState(intArrayOf(), shape(ReaderStyle.POPUP_BOX))
        }
    }

    /** The word, its reading, how it was inflected, and what frequency and pitch lists say. */
    private fun headline(result: LookupResult): CharSequence = SpannableStringBuilder().apply {
        append(result.expression, StyleSpan(Typeface.BOLD), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val small = (px(SMALL_SP)).toInt()
        fun addSmall(text: String, colour: Int) {
            val start = length
            append("  ").append(text)
            setSpan(AbsoluteSizeSpan(small), start, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            setSpan(ForegroundColorSpan(colour), start, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        if (result.displayReading.isNotEmpty()) addSmall(result.displayReading, ReaderStyle.POPUP_DIM)
        if (result.reasons.isNotEmpty()) addSmall("« " + result.reasons.joinToString(" « "), ReaderStyle.POPUP_ACCENT)
        for (pitch in result.pitches) addSmall(pitch.positions.joinToString(",", "[", "]"), ReaderStyle.POPUP_ACCENT)
        for (frequency in result.frequencies) {
            addSmall("${frequency.dictionary} ${frequency.display}", ReaderStyle.POPUP_DIM)
        }
    }

    /**
     * One line per sense: its number, its tags where they differ from the sense before, its
     * glosses run together, and the dictionary's notes on it. With entries from several
     * dictionaries, each dictionary's senses are headed by its name.
     */
    private fun senses(result: LookupResult): CharSequence = SpannableStringBuilder().apply {
        val small = px(SMALL_SP).toInt()
        fun dim(text: String) {
            val start = length
            append(text)
            setSpan(AbsoluteSizeSpan(small), start, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            setSpan(ForegroundColorSpan(ReaderStyle.POPUP_DIM), start, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        val entries = result.entries.filterNot(::isFormsList)
        val several = entries.map { it.dictionary }.distinct().size > 1
        var dictionary: String? = null
        var number = 0
        var lastTags = ""
        for (entry in entries) {
            val definitions = GlossaryRenderer.compact(entry.glossaryJson)
            if (definitions.isEmpty()) continue
            if (isNotEmpty()) append('\n')
            if (entry.dictionary != dictionary) {
                dictionary = entry.dictionary
                number = 0
                lastTags = ""
                if (several) {
                    dim(entry.dictionary)
                    append('\n')
                }
            }
            number++
            // Numbers among the tags only repeat the order of the senses.
            val tags = entry.definitionTags.split(' ').filter { it.isNotBlank() && it.toIntOrNull() == null }
                .joinToString(",")
            dim("$number ")
            if (tags.isNotEmpty() && tags != lastTags) dim("$tags ")
            lastTags = tags
            append(definitions.joinToString(" | ") { it.glosses })
            val notes = definitions.map { it.notes }.filter { it.isNotEmpty() }.joinToString("; ")
            if (notes.isNotEmpty()) dim(" ($notes)")
        }
    }

    /** JMdict lists a word's other spellings as an entry of its own; that is not a sense. */
    private fun isFormsList(entry: TermEntry): Boolean =
        entry.definitionTags.split(' ').any { it == "forms" }

    private fun label(text: String, sizeSp: Float, color: Int) = TextView(context).apply {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_PX, px(sizeSp))
        setTextColor(color)
        includeFontPadding = false
    }

    /** A text size in pixels, with the popup's own scale applied. */
    private fun px(sizeSp: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, sizeSp, resources.displayMetrics) * textScale / 100f

    private fun dp(value: Float) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)

    private companion object {
        const val HEAD_SP = 17f
        const val SENSE_SP = 13f
        const val SMALL_SP = 11f
        const val BOX_DP = 24f
        const val BOTTOM_DP = 8f
    }
}
