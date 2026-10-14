// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.os.SystemClock
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.ProfileStatus
import org.ppsspp.ppsspp.reader.ReaderNative
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.cursor.HoverCursor
import org.ppsspp.ppsspp.reader.mining.Miner
import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.screen.Area
import org.ppsspp.ppsspp.reader.screen.PlacedText
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.stats.SessionSummary

/**
 * The reader's in-game layer for one display: the invisible layer that makes the game's own
 * text something to tap, the text panel with the current line for when that is not possible,
 * the dictionary popup and, on the game's display, a small line about the reading session in
 * the top right corner. It fills its parent and lets touches outside its own surfaces through
 * to the game.
 *
 * While the cursor's button is held ([HoverCursor]), the second display is dimmed and is a
 * trackpad (without one, the game's display is, undimmed), and the overlay of the game's
 * display draws the cursor and looks up the word it rests on.
 */
@SuppressLint("ViewConstructor")
class ReaderOverlay(
    context: Context,
    private val host: Host,
    /** True for the overlay on the second display of a device with two: the trackpad. */
    private val secondary: Boolean
) : FrameLayout(context),
    ReaderSession.Listener,
    SharedPreferences.OnSharedPreferenceChangeListener,
    HoverCursor.Surface {

    /** What the overlay needs from the screen that owns it. */
    interface Host {
        fun openBacklog()
        fun openStatistics()
        fun openSettings()
        fun openDictionaries()
        fun openAnkiSetup()

        /** Lets the user say what the running game's cards do with their line's audio. */
        fun chooseCardAudio()

        /** True when a second screen is showing the game, so its overlay should hold the panel. */
        fun hasSecondaryScreen(): Boolean
    }

    private val settings = ReaderSettings(context)
    private val miner = Miner(context)

    private val panel = LinearLayout(context)
    private val speakerView = TextView(context)
    private val textScroll = MaxHeightScrollView(context)
    private val textView = ReaderTextView(context)
    private val choicesView = ReaderTextView(context)
    private val handle = ImageButton(context)
    private val sessionView = TextView(context)

    /** Two bars: the statistics are paused. */
    private val pauseIcon = object : android.graphics.drawable.Drawable() {
        private val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = ReaderStyle.ACCENT
            setShadowLayer(dp(2f).toFloat(), 0f, 0f, ReaderStyle.SESSION_SHADOW)
        }

        override fun draw(canvas: Canvas) {
            val box = bounds
            val bar = box.width() * 0.3f
            val top = box.top + box.height() * 0.1f
            val bottom = box.bottom - box.height() * 0.1f
            canvas.drawRect(box.left + box.width() * 0.12f, top, box.left + box.width() * 0.12f + bar, bottom, paint)
            canvas.drawRect(box.right - box.width() * 0.12f - bar, top, box.right - box.width() * 0.12f, bottom, paint)
        }

        override fun setAlpha(alpha: Int) {
            paint.alpha = alpha
        }

        override fun setColorFilter(filter: android.graphics.ColorFilter?) {
            paint.colorFilter = filter
        }

        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = android.graphics.PixelFormat.TRANSLUCENT
    }

    /** On the second display: what the display is for, and that the cursor has it. */
    private val padHint = TextView(context)
    private val screenText = ScreenTextLayer(context, secondary)
    private val lookups = LookupLayer(context) { host.openDictionaries() }

    private var shown: PresentedLine? = null
    private var shownChoices: PresentedLine? = null
    private var userHidden = false

    /** The shown line has been tappable in the game's own picture, on whichever screen. */
    private var shownPlaced = false

    /** Until then (uptime), a line the game should place is given time to be placed. */
    private var placementDue = 0L

    /** The open popup was asked from the game's own picture rather than from the panel. */
    private var lookupInGame = false

    /** What the reading session holds so far, once something was counted. */
    private var session: ReadingCounters? = null

    /** The view the game is drawn in on this screen. */
    var gameSurface: View?
        get() = screenText.surface
        set(value) {
            screenText.surface = value
        }

    init {
        layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT)
        buildScreenText()
        lookups.holdOpen = { cursorOnPopupOrItsWord() }
        buildSessionView()
        if (secondary) buildPadHint()
        buildPanel()
        buildHandle()
        lookups.passThrough = panel
        lookups.passThroughAt = { x, y -> screenText.isTextBox(x, y) }
        lookups.onClosed = { lookupInGame = false }
        addView(lookups)
        applySettings()
    }

    // ---- Construction ----

    private fun buildScreenText() {
        screenText.listener = object : ScreenTextLayer.Listener {
            override fun onCharacterTap(text: PlacedText, character: PlacedText.Glyph) =
                lookUpInGame(text, character)

            override fun onBoxTap(text: PlacedText) {
                lookups.close()
            }
        }
        addView(screenText, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    }

    /** The session line: text only, and no touch stops at it. */
    private fun buildSessionView() {
        sessionView.apply {
            setTextColor(ReaderStyle.SESSION_TEXT)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setShadowLayer(dp(2f).toFloat(), 0f, 0f, ReaderStyle.SESSION_SHADOW)
            includeFontPadding = false
            maxLines = 1
            compoundDrawablePadding = dp(4f)
            visibility = GONE
        }
        pauseIcon.setBounds(0, 0, dp(11f), dp(11f))
        addView(
            sessionView,
            LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END).apply {
                setMargins(0, dp(4f), dp(8f), 0)
            }
        )
    }

    /**
     * The second display says what it is when it becomes the trackpad, for a moment. Without
     * a game's text to point at it is black.
     */
    private fun buildPadHint() {
        padHint.apply {
            id = R.id.reader_pad_hint
            setTextColor(ReaderStyle.PAD_HINT)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            includeFontPadding = false
            gravity = Gravity.CENTER
        }
        addView(padHint, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.CENTER))
    }

    private val fadePadHint = Runnable { padHint.animate().alpha(0f).setDuration(PAD_HINT_FADE_MS).start() }

    private fun showPadHint(@Suppress("UNUSED_PARAMETER") remind: Boolean) {
        if (!secondary) return
        padHint.animate().cancel()
        removeCallbacks(fadePadHint)
        if (cursorOn) {
            padHint.setText(R.string.reader_pad_active)
            padHint.setTextColor(ReaderStyle.PAD_HINT_ACTIVE)
            padHint.alpha = 1f
            // A trackpad that is there all the time says so once and is then only itself.
            if (HoverCursor.always) postDelayed(fadePadHint, PAD_HINT_MS)
            return
        }
        // Without text to point at the display is dark and says nothing: there is no button
        // to tell of, the trackpad comes by itself.
        padHint.text = ""
        padHint.alpha = 0f
    }

    private fun buildPanel() {
        panel.orientation = LinearLayout.VERTICAL
        panel.isClickable = true // the panel itself never passes taps to the game
        val pad = dp(8f)
        panel.setPadding(pad + dp(4f), pad / 2, pad, pad)

        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        speakerView.apply {
            setTextColor(ReaderStyle.ACCENT)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            maxLines = 1
            includeFontPadding = false
        }
        header.addView(speakerView, LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
        header.addView(button(R.drawable.ic_reader_backlog, R.string.reader_backlog) { host.openBacklog() })
        header.addView(button(R.drawable.ic_reader_card, R.string.reader_mine_sentence) { mineSentence(it) })
        header.addView(button(R.drawable.ic_reader_x_more_vert, R.string.reader_more) { showMenu(it) })
        header.addView(button(R.drawable.ic_reader_down, R.string.reader_hide_panel) { setUserHidden(true) })
        header.addView(
            button(R.drawable.ic_reader_next, R.string.reader_advance) { ReaderSession.pressAdvance() }
                .apply { imageTintList = ColorStateList.valueOf(ReaderStyle.ACCENT) }
        )
        panel.addView(header)

        textView.setTextColor(ReaderStyle.TEXT)
        textView.wordHighlightColor = ReaderStyle.HIGHLIGHT
        textView.setLineSpacing(0f, 1.1f)
        textView.onCharacterTap = { offset, bounds ->
            shown?.let { lookupInPanel(textView, it, offset, bounds) }
        }
        textView.onLongPress = { copyLine() }
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        column.addView(textView)

        choicesView.setTextColor(ReaderStyle.ACCENT)
        choicesView.wordHighlightColor = ReaderStyle.HIGHLIGHT
        choicesView.setPadding(0, dp(6f), 0, 0)
        choicesView.visibility = GONE
        choicesView.onCharacterTap = { offset, bounds ->
            shownChoices?.let { lookupInPanel(choicesView, it, offset, bounds) }
        }
        column.addView(choicesView)

        textScroll.addView(column, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        panel.addView(textScroll, LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))

        panel.visibility = GONE
        addView(panel, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
    }

    /**
     * A lookup in the game's own text: the popup goes beside the text's rows, at the word.
     * [quiet] when it was not asked for by hand (see [LookupLayer.lookup]).
     */
    private fun lookUpInGame(text: PlacedText, character: PlacedText.Glyph, quiet: Boolean = false) {
        lookupInGame = true
        lookups.lookup(
            screenText.targetFor(text.presented),
            text.presented,
            character.offset,
            LookupLayer.Anchor(text.rows, character.drawn.left),
            quiet
        )
    }

    /** A lookup from the panel: the popup goes beside the panel, at the tapped word. */
    private fun lookupInPanel(
        view: ReaderTextView,
        presented: PresentedLine,
        offset: Int,
        bounds: Rect,
        quiet: Boolean = false
    ) {
        lookupInGame = false
        val nearX = LookupLayer.areaOf(view, lookups).left + bounds.left
        lookups.lookup(view, presented, offset, LookupLayer.Anchor(LookupLayer.areaOf(panel, lookups), nearX), quiet)
    }

    private fun buildHandle() {
        handle.setImageResource(R.drawable.ic_reader_open_book)
        handle.imageTintList = ColorStateList.valueOf(ReaderStyle.TEXT)
        handle.background = GradientDrawable().apply {
            setColor(0x99000000.toInt())
            cornerRadius = dp(20f).toFloat()
        }
        handle.contentDescription = context.getString(R.string.reader_show_panel)
        handle.setOnClickListener { setUserHidden(false) }
        handle.visibility = GONE
        addView(handle, LayoutParams(dp(40f), dp(40f)))
    }

    private fun button(icon: Int, description: Int, action: (View) -> Unit) = ImageButton(context).apply {
        setImageResource(icon)
        imageTintList = ColorStateList.valueOf(ReaderStyle.TEXT)
        setBackgroundColor(Color.TRANSPARENT)
        contentDescription = context.getString(description)
        setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
        layoutParams = LinearLayout.LayoutParams(dp(40f), dp(40f))
        setOnClickListener(action)
    }

    // ---- Lifecycle ----

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        ReaderSession.addListener(this)
        HoverCursor.attach(this)
        if (!secondary && width > 0) HoverCursor.setBounds(width, height)
        settings.preferences.registerOnSharedPreferenceChangeListener(this)
        session = ReaderSession.sessionActivity
        applySettings()
        ReaderSession.current?.let { showLine(it) }
        ReaderSession.currentChoices?.let { showChoices(it) }
        screenText.refresh()
        HoverCursor.refresh()
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(placementCheck)
        removeCallbacks(hoverLookup)
        removeCallbacks(fadePadHint)
        HoverCursor.detach(this)
        cursorOn = false
        ReaderSession.removeListener(this)
        settings.preferences.unregisterOnSharedPreferenceChangeListener(this)
        super.onDetachedFromWindow()
    }

    override fun onSharedPreferenceChanged(prefs: SharedPreferences?, key: String?) {
        // No key: the preferences were cleared, which changes all of them.
        if (key == null || key.startsWith("reader_")) {
            applySettings()
            ReaderSession.onSettingsChanged()
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        applySizes()
        if (!secondary) HoverCursor.setBounds(w, h)
        if (cursorOn && secondary) trackpad = screenText.screenAreas(ScreenLayout.SCREEN_PAD)
        HoverCursor.refresh()
    }

    /** Call when the second screen appears or goes away. */
    fun refresh() {
        applySettings()
        HoverCursor.refresh()
    }

    // ---- ReaderSession.Listener ----

    override fun onLine(presented: PresentedLine) {
        if (presented.line.isChoice) {
            showChoices(presented)
        } else {
            lookups.close()
            showLine(presented)
        }
    }

    override fun onScreenTextChanged() {
        screenText.refresh()
        val line = shown
        if (line != null && !shownPlaced && ReaderSession.placementOf(line) != null) {
            shownPlaced = true
            updateVisibility()
        }
        val placed = screenText.texts.map { it.presented.backlogId }
        val fresh = placed.any { it !in placedLines }
        placedLines = placed
        if (cursorOn && !secondary) {
            homeCursor()
            if (!cursorHome && fresh) {
                // The cursor stayed where it was and a new text came under it: nobody pointed
                // at what is there now.
                pointed = false
                removeCallbacks(hoverLookup)
                hovered = null
                resting = null
            }
            hover()
        }
    }

    /** Settings that touch the session changed: the statistics were paused or resumed. */
    override fun onActivityChanged() {
        showSession()
        HoverCursor.refresh()
    }

    override fun onSessionActivity(counters: ReadingCounters?) {
        session = counters
        showSession()
    }

    /** The session so far in the top right corner of the main screen, for those who want it. */
    private fun showSession() {
        val counters = session
        val line = settings.sessionHud && counters != null
        // Paused statistics show as a small mark in the corner, with or without the line.
        val paused = settings.statsPaused
        val wanted = !secondary && (line || paused)
        val mark = paused && !secondary
        if (mark != pauseShown) {
            pauseShown = mark
            sessionView.setCompoundDrawables(if (mark) pauseIcon else null, null, null, null)
            sessionView.contentDescription =
                if (mark) context.getString(R.string.reader_stats_paused_icon) else null
        }
        if (wanted) {
            val text = if (line) SessionSummary.format(counters!!) else ""
            if (sessionView.text.toString() != text) sessionView.text = text
        }
        sessionView.visibility = if (wanted) VISIBLE else GONE
    }

    /** True while the mark for paused statistics is in the corner (for tests too). */
    var pauseShown = false
        private set

    override fun onProfileStatus(status: ProfileStatus) {
        // A game with text to point at has begun, or ended: the second display's trackpad
        // comes and goes with it.
        HoverCursor.refresh()
        if (!holdsPanel() || shown != null) return
        val message = when {
            status.error.isNotEmpty() -> context.getString(R.string.reader_profile_error, status.error)
            !status.loaded -> context.getString(R.string.reader_no_profile, status.titleId)
            else -> return
        }
        Toast.makeText(context, message, Toast.LENGTH_LONG).show()
    }

    override fun onCommand(command: ReaderSession.Command) {
        when (command) {
            ReaderSession.Command.TOGGLE_PANEL ->
                if (holdsPanel() && shown != null) setUserHidden(!userHidden)
            // These work with or without a panel; one overlay answers, the main screen's.
            ReaderSession.Command.SENTENCE_CARD -> if (!secondary) mineSentence(panel)
            ReaderSession.Command.BACKLOG -> if (!secondary) host.openBacklog()
        }
    }

    // ---- Presentation ----

    /** True when this overlay, rather than the one on the other screen, shows the panel. */
    private fun holdsPanel(): Boolean {
        if (!settings.panelEnabled) return false
        return when (settings.panelDisplay) {
            ReaderSettings.DISPLAY_PRIMARY -> !secondary
            ReaderSettings.DISPLAY_SECONDARY -> secondary == host.hasSecondaryScreen()
            else -> secondary == host.hasSecondaryScreen()
        }
    }

    /**
     * False while the line is better read in the game's own picture: its text can be tapped
     * there (on either screen), or the game is about to say where it is.
     */
    private fun panelNeeded(): Boolean {
        val line = shown ?: return false
        if (!settings.panelAutoHide || !settings.screenText) return true
        if (line.line.window.isEmpty()) return true // this game does not say where text is
        if (shownPlaced) return false
        return SystemClock.uptimeMillis() >= placementDue
    }

    private val placementCheck = Runnable { updateVisibility() }

    private fun applySettings() {
        screenText.active = settings.screenText
        screenText.outline = settings.screenTextOutline
        val top = settings.panelPosition == ReaderSettings.POSITION_TOP
        val edge = if (top) Gravity.TOP else Gravity.BOTTOM
        (panel.layoutParams as LayoutParams).apply {
            gravity = edge or Gravity.CENTER_HORIZONTAL
            val margin = dp(6f)
            setMargins(margin, margin, margin, margin)
        }
        (handle.layoutParams as LayoutParams).apply {
            gravity = edge or Gravity.END
            val margin = dp(10f)
            setMargins(margin, margin, margin, margin)
        }
        panel.background = GradientDrawable().apply {
            val alpha = settings.panelOpacityPercent * 255 / 100
            setColor((ReaderStyle.PANEL_BACKGROUND and 0x00FFFFFF) or (alpha shl 24))
            cornerRadius = dp(10f).toFloat()
        }
        textView.setTextSize(TypedValue.COMPLEX_UNIT_SP, settings.fontSizeSp.toFloat())
        choicesView.setTextSize(TypedValue.COMPLEX_UNIT_SP, settings.fontSizeSp * 0.85f)
        applySizes()
        shown?.let { showLine(it) }
        shownChoices?.let { showChoices(it) }
        updateVisibility()
        showSession()
        showPadHint(remind = true)
        cursorFrame = settings.cursorFrame
        cursorHome = settings.cursorHome
        if (cursorOn) invalidate()
    }

    private fun applySizes() {
        if (height == 0) return
        // The cap is for the whole panel: take off the button row and the panel's padding.
        val chrome = dp(40f) + panel.paddingTop + panel.paddingBottom
        textScroll.maxHeight = (height * settings.panelHeightPercent / 100 - chrome).coerceAtLeast(dp(40f))
    }

    private fun updateVisibility() {
        val active = holdsPanel() && shown != null && panelNeeded()
        panel.visibility = if (active && !userHidden) VISIBLE else GONE
        handle.visibility = if (active && userHidden) VISIBLE else GONE
        // A popup asked from the panel goes with the panel; one asked from the game's own
        // text stays.
        if ((!active || userHidden) && !lookupInGame) lookups.close()
    }

    private fun setUserHidden(hidden: Boolean) {
        userHidden = hidden
        updateVisibility()
    }

    private fun showLine(presented: PresentedLine) {
        if (shown?.backlogId != presented.backlogId) {
            shownPlaced = ReaderSession.placementOf(presented) != null
            placementDue = SystemClock.uptimeMillis() + PLACEMENT_GRACE_MS
            removeCallbacks(placementCheck)
            if (!shownPlaced) postDelayed(placementCheck, PLACEMENT_GRACE_MS + 20)
        }
        shown = presented
        shownChoices = null
        choicesView.visibility = GONE
        val line = presented.line
        speakerView.text = line.speaker
        textView.setTextColor(
            if (settings.rereadColour && presented.accounting.reread) ReaderStyle.TEXT_REREAD else ReaderStyle.TEXT
        )
        textView.setLine(line.text, line.ruby, settings.showFurigana)
        textScroll.scrollTo(0, 0)
        updateVisibility()
    }

    private fun showChoices(presented: PresentedLine) {
        if (!settings.showChoices) return
        shownChoices = presented
        choicesView.setLine(presented.line.text, presented.line.ruby, settings.showFurigana)
        choicesView.visibility = VISIBLE
        if (shown == null) {
            // Choices can come before any dialogue; the panel still needs something to hold.
            shown = presented
            speakerView.text = ""
            textView.setLine("", emptyList(), false)
        }
        updateVisibility()
    }

    // ---- The cursor ----

    /** Whether the cursor was in use when this overlay last heard of it. */
    private var cursorOn = false

    /** Where on this overlay a finger moves the cursor. */
    internal var trackpad: List<Area> = emptyList()
        private set

    // The finger on the trackpad.
    private var padPointer = -1
    private var padArea: Area? = null
    private var padLifted = false
    private var padMoved = false
    private var padX = 0f
    private var padY = 0f
    private var padDownX = 0f
    private var padDownY = 0f
    private var padDownTime = 0L
    private var padCursorX = 0f
    private var padCursorY = 0f

    /** A click of the cursor is being passed to the views: it is not a touch on the trackpad. */
    private var clicking = false

    /** When the player last moved the cursor or clicked with it (see [drawCursor]). */
    private var cursorMoved = 0L
    private val redraw = Runnable { invalidate() }

    /** The shape of the second display's trackpad. */
    private val padShape = Path()
    private val padRect = RectF()

    // How the cursor behaves over text (settings, see ReaderSettings).
    private var cursorFrame = true
    private var cursorHome = true

    /** The cursor is being put somewhere by the reader, not moved by the player. */
    private var placing = false

    /**
     * What the cursor rests on is something the player pointed at. Not so for a cursor that
     * was left where it was while a new line came, or that was just switched on again: what
     * happens to be under it is only looked up once the cursor has been moved.
     */
    private var pointed = true

    /** The lines whose text is placed in the game's picture, to notice a new one. */
    private var placedLines: List<Long> = emptyList()

    /** What the cursor is over, and what is looked up once it has rested there. */
    private sealed class Hover(val key: Any) {
        class InGame(val text: PlacedText, val character: PlacedText.Glyph) :
            Hover(text.presented.backlogId to character.offset)

        class InPanel(val view: ReaderTextView, val presented: PresentedLine, val offset: Int) :
            Hover(Triple(view, presented.backlogId, offset))
    }

    private var hovered: Any? = null
    private var resting: Hover? = null
    private val hoverLookup = Runnable {
        val target = resting
        if (cursorOn && target != null) {
            // Quietly: a cursor resting on a comma is no reason to say that a comma is no word.
            when (target) {
                is Hover.InGame -> lookUpInGame(target.text, target.character, quiet = true)

                is Hover.InPanel -> lookupInPanel(
                    target.view,
                    target.presented,
                    target.offset,
                    target.view.boundsOf(target.offset),
                    quiet = true
                )
            }
        }
    }

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val dimPaint = Paint().apply { color = ReaderStyle.TRACKPAD_DIM }
    private val padPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val cursorPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun hasTrackpad(): Boolean = screenText.screenAreas(ScreenLayout.SCREEN_PAD).isNotEmpty()

    override fun isSecondDisplay(): Boolean = secondary && hasTrackpad()

    override fun onCursorChanged() {
        val on = HoverCursor.active
        if (on != cursorOn) {
            cursorOn = on
            if (on) {
                screenText.refresh()
                trackpad = when {
                    !HoverCursor.everywhere -> screenText.screenAreas(ScreenLayout.SCREEN_PAD)
                    secondary -> emptyList()
                    else -> listOf(Area(0f, 0f, width.toFloat(), height.toFloat()))
                }
                // Put at the start of the text it points at nothing yet; left where it was,
                // it has not been pointed anywhere this time.
                pointed = cursorHome
                if (!secondary) homeCursor()
            } else {
                trackpad = emptyList()
                removeCallbacks(hoverLookup)
                hovered = null
                resting = null
            }
            showPadHint(remind = false)
            invalidate()
        } else if (on && !placing && !pointed) {
            // Moved by the player: from here on, what it rests on was pointed at.
            pointed = true
            hovered = null
        }
        // Only the main screen shows where the cursor is.
        if (on && !secondary) {
            if (!placing) cursorMoved = SystemClock.uptimeMillis()
            invalidate()
            hover()
        }
    }

    override fun onCursorClick() {
        if (secondary || !cursorOn) return
        cursorMoved = SystemClock.uptimeMillis()
        // A tap at the cursor, for whichever of the reader's views is there: a word, the
        // popup's box, or nothing, which closes the popup as a tap beside it would.
        val now = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, HoverCursor.x, HoverCursor.y, 0)
        val up = MotionEvent.obtain(now, now + 40, MotionEvent.ACTION_UP, HoverCursor.x, HoverCursor.y, 0)
        clicking = true
        try {
            super.dispatchTouchEvent(down)
            super.dispatchTouchEvent(up)
        } finally {
            clicking = false
            down.recycle()
            up.recycle()
        }
    }

    /**
     * A new text has the cursor at its start, just before the first character, so that the way
     * to a word is always the same. Within one text the cursor stays where it was left. As a
     * setting it stays where it was left throughout, and only starts out in the middle of the
     * game's picture.
     */
    private fun homeCursor() {
        val text = if (cursorHome) screenText.texts.firstOrNull() else null
        placing = true
        try {
            if (text != null) {
                val line = text.presented.backlogId
                if (HoverCursor.homedFor == line) return
                val first = text.characters.first()
                HoverCursor.home(first.touch.left - 1f, (first.drawn.top + first.drawn.bottom) / 2f, line)
            } else if (HoverCursor.homedFor == null) {
                val screen = screenText.screenAreas(ScreenLayout.SCREEN_GAME).firstOrNull()
                    ?: Area(0f, 0f, width.toFloat(), height.toFloat())
                HoverCursor.home((screen.left + screen.right) / 2f, (screen.top + screen.bottom) / 2f, NO_LINE)
            }
        } finally {
            placing = false
        }
    }

    /** Looks up the word under the cursor once the cursor has rested on it for a moment. */
    private fun hover() {
        val x = HoverCursor.x
        val y = HoverCursor.y
        lookups.cursorMoved(cursorOnPopupOrItsWord())
        if (!pointed || lookups.isAsking || lookups.popupContains(x, y)) {
            removeCallbacks(hoverLookup)
            return
        }
        val target = under(x, y)
        if (target?.key == hovered) return
        hovered = target?.key
        resting = target
        removeCallbacks(hoverLookup)
        if (target != null) postDelayed(hoverLookup, HOVER_REST_MS)
    }

    /** True while the cursor is on the open popup or on the word the popup is about. */
    private fun cursorOnPopupOrItsWord(): Boolean {
        if (!cursorOn || !lookups.isOpen) return false
        val x = HoverCursor.x
        val y = HoverCursor.y
        if (lookups.popupContains(x, y)) return true
        return when (val target = under(x, y)) {
            is Hover.InGame -> lookups.isAbout(target.text.presented, target.character.offset)
            is Hover.InPanel -> lookups.isAbout(target.presented, target.offset)
            null -> false
        }
    }

    private fun under(x: Float, y: Float): Hover? {
        screenText.characterAt(x, y)?.let { return Hover.InGame(it.text, it.character) }
        if (panel.visibility != VISIBLE) return null
        for ((view, line) in listOf(textView to shown, choicesView to shownChoices)) {
            if (line == null || view.visibility != VISIBLE) continue
            val area = LookupLayer.areaOf(view, this)
            if (!area.contains(x, y)) continue
            val offset = view.characterAt(x - area.left, y - area.top)
            if (offset >= 0) return Hover.InPanel(view, line, offset)
        }
        return null
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (clicking) return super.dispatchTouchEvent(event)
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            padArea = if (HoverCursor.active) trackpad.firstOrNull { it.contains(event.x, event.y) } else null
            padPointer = -1
            if (padArea != null) {
                // The whole gesture is the trackpad's, to its end: nothing of it is the game's.
                padPointer = event.getPointerId(0)
                padLifted = false
                padMoved = false
                padX = event.x
                padY = event.y
                padDownX = padX
                padDownY = padY
                padDownTime = event.eventTime
                padCursorX = HoverCursor.x
                padCursorY = HoverCursor.y
                if (secondary) invalidate()
                return true
            }
        } else if (padPointer != -1) {
            onTrackpad(event)
            if (secondary) invalidate()
            return true
        }
        return super.dispatchTouchEvent(event)
    }

    private fun onTrackpad(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_MOVE -> {
                val index = event.findPointerIndex(padPointer)
                if (index < 0 || padLifted) return
                val x = event.getX(index)
                val y = event.getY(index)
                if (kotlin.math.abs(x - padDownX) > touchSlop || kotlin.math.abs(y - padDownY) > touchSlop) {
                    padMoved = true
                }
                // The cursor follows from the first pixel: a small correction is a small stroke.
                if (HoverCursor.active) {
                    val gain = trackpadGain()
                    HoverCursor.moveBy((x - padX) * gain, (y - padY) * gain)
                }
                padX = x
                padY = y
            }

            MotionEvent.ACTION_POINTER_UP ->
                if (event.getPointerId(event.actionIndex) == padPointer) padLifted = true

            MotionEvent.ACTION_UP -> {
                if (!padMoved && !padLifted && event.eventTime - padDownTime < TAP_MS && HoverCursor.active) {
                    // A tap: what the finger wobbled meanwhile is taken back, so that the click
                    // lands where the cursor was put.
                    HoverCursor.moveBy(padCursorX - HoverCursor.x, padCursorY - HoverCursor.y)
                    HoverCursor.click()
                }
                padPointer = -1
            }

            MotionEvent.ACTION_CANCEL -> padPointer = -1
        }
    }

    /**
     * How far the cursor goes for a pixel of finger: a stroke across the trackpad takes it
     * across the game's display. Where the game's display itself is the trackpad, one for one.
     */
    private fun trackpadGain(): Float {
        val speed = HoverCursor.speedPercent / 100f
        val area = padArea
        if (HoverCursor.everywhere || area == null || area.width <= 0f || HoverCursor.width <= 0) return speed
        return HoverCursor.width / area.width * speed
    }

    override fun dispatchDraw(canvas: Canvas) {
        // The trackpad's look lies under the reader's own views, the cursor above them.
        if (cursorOn && !HoverCursor.everywhere) {
            for (area in trackpad) {
                if (secondary) {
                    drawPad(canvas, area)
                } else {
                    canvas.drawRect(area.left, area.top, area.right, area.bottom, dimPaint)
                }
            }
        }
        super.dispatchDraw(canvas)
        if (cursorOn && !secondary) {
            // A cursor that is there all the time is shown while it is being used, and for a
            // while after: left alone it would lie on the game's picture for hours.
            val idle = SystemClock.uptimeMillis() - cursorMoved
            if (!HoverCursor.always || idle < CURSOR_IDLE_MS) {
                drawCursor(canvas, HoverCursor.x, HoverCursor.y)
                if (HoverCursor.always) {
                    removeCallbacks(redraw)
                    postDelayed(redraw, CURSOR_IDLE_MS - idle + 20)
                }
            }
        }
    }

    /**
     * The second display while it is the trackpad. It is black otherwise, so there is nothing
     * to dim: it lights up a little instead, with an edge.
     */
    private fun drawPad(canvas: Canvas, area: Area) {
        val inset = dp(10f).toFloat()
        val corner = dp(22f).toFloat()
        padRect.set(area.left + inset, area.top + inset, area.right - inset, area.bottom - inset)
        if (padRect.width() <= 0f || padRect.height() <= 0f) return
        // The console's own ground, as the word popup has it ([ConsoleLook]), under a veil: it
        // is a surface for a thumb beside the game, not a second picture.
        // (made anew each time: one that has drawn before draws fainter, its paint being left
        // as its last stroke had it)
        val ground = ConsoleLook.ground(resources.displayMetrics.density)
        padShape.rewind()
        padShape.addRoundRect(padRect, corner, corner, Path.Direction.CW)
        canvas.save()
        canvas.clipPath(padShape)
        ground.setBounds(padRect.left.toInt(), padRect.top.toInt(), padRect.right.toInt(), padRect.bottom.toInt())
        ground.draw(canvas)
        padPaint.style = Paint.Style.FILL
        padPaint.color = ReaderStyle.PAD_VEIL
        canvas.drawRect(padRect, padPaint)
        // Where the finger is: the console's pointer, as on the game.
        if (padPointer != -1 && !padLifted) {
            ConsoleLook.drawPointer(canvas, cursorPaint, padX, padY, maxOf(dp(44f).toFloat(), height / 9f))
        }
        canvas.restore()
        padPaint.style = Paint.Style.STROKE
        padPaint.strokeWidth = dp(1.5f).toFloat()
        padPaint.color = ReaderStyle.PAD_EDGE
        canvas.drawRoundRect(padRect, corner, corner, padPaint)
    }

    /**
     * On a character of the game's text the cursor is a frame around that character: it shows
     * which one a lookup would start at and covers none of it. Anywhere else it is the
     * console's pointer ([ConsoleLook]); as a setting, it is that everywhere.
     */
    private fun drawCursor(canvas: Canvas, x: Float, y: Float) {
        val edge = dp(1f).toFloat().coerceAtLeast(1f)
        val line = dp(2f).toFloat()
        cursorPaint.style = Paint.Style.STROKE
        val cell = if (cursorFrame) screenText.characterAt(x, y)?.character?.drawn else null
        if (cell != null) {
            val room = dp(1f).toFloat()
            val corner = dp(3f).toFloat()
            for (pass in 0..1) {
                cursorPaint.color = if (pass == 0) ReaderStyle.CURSOR_EDGE else ReaderStyle.CURSOR
                cursorPaint.strokeWidth = if (pass == 0) line + 2 * edge else line
                canvas.drawRoundRect(
                    cell.left - room,
                    cell.top - room,
                    cell.right + room,
                    cell.bottom + room,
                    corner,
                    corner,
                    cursorPaint
                )
            }
            return
        }
        // The console's pointer, as the desktop app's: seen from a sofa as well as from a desk.
        ConsoleLook.drawPointer(canvas, cursorPaint, x, y, maxOf(dp(30f).toFloat(), height / 20f))
    }

    // ---- Cards ----

    private var mining = false

    private fun mineSentence(@Suppress("UNUSED_PARAMETER") source: View) {
        val presented = shown ?: return
        if (mining) return // one card per press, also when a button is held or bounces
        mining = true
        miner.mine(presented, null, null, lookups.audioQuestion) { outcome ->
            mining = false
            LookupLayer.report(context, outcome)
        }
    }

    // ---- Menu ----

    private fun copyLine() {
        val line = shown?.line ?: return
        ReaderSession.copyToClipboard(line.text)
        Toast.makeText(context, R.string.reader_line_copied, Toast.LENGTH_SHORT).show()
    }

    private fun showMenu(anchor: View) {
        val menu = PopupMenu(context, anchor)
        val paused = settings.statsPaused
        menu.menu.add(0, 1, 0, R.string.reader_copy_line)
        menu.menu.add(0, 2, 1, R.string.reader_statistics)
        menu.menu.add(0, 3, 2, if (paused) R.string.reader_resume_tracking else R.string.reader_pause_tracking)
        menu.menu.add(0, 4, 3, R.string.reader_dictionaries)
        menu.menu.add(0, 5, 4, R.string.reader_anki_setup)
        menu.menu.add(0, 6, 5, R.string.reader_settings)
        menu.menu.add(0, 7, 6, R.string.reader_reload_profile)
        menu.menu.add(0, 8, 7, R.string.reader_card_audio_game)
        menu.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                1 -> copyLine()
                2 -> host.openStatistics()
                3 -> ReaderSession.toggleStatsPaused()
                4 -> host.openDictionaries()
                5 -> host.openAnkiSetup()
                6 -> host.openSettings()
                7 -> ReaderNative.reloadProfile()
                8 -> host.chooseCardAudio()
            }
            true
        }
        menu.show()
    }

    private fun dp(value: Float): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics).toInt()

    private companion object {
        /** How long a line may take to be placed in the game's picture before the panel shows
         * it instead. The game reports the place within a frame or two of the text. */
        const val PLACEMENT_GRACE_MS = 400L

        /** How long the cursor rests on a character before the word there is looked up. */
        const val HOVER_REST_MS = 140L

        /** A touch on the trackpad that ends sooner than this without moving is a click. */
        const val TAP_MS = 300L

        /** "No line": the cursor was put in the middle of the screen, there being no text. */
        const val NO_LINE = -1L

        /** How long the second display says how the cursor is called up, and how fast it goes. */
        /** How long a cursor that is there all the time stays in the picture once left alone. */
        const val CURSOR_IDLE_MS = 4000L

        const val PAD_HINT_MS = 6000L
        const val PAD_HINT_FADE_MS = 800L
    }

    /** A ScrollView that grows with its content up to [maxHeight]. */
    private class MaxHeightScrollView(context: Context) : ScrollView(context) {
        var maxHeight: Int = Int.MAX_VALUE
            set(value) {
                if (field != value) {
                    field = value
                    requestLayout()
                }
            }

        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
            super.onMeasure(
                widthMeasureSpec,
                MeasureSpec.makeMeasureSpec(maxHeight.coerceAtMost(1 shl 29), MeasureSpec.AT_MOST)
            )
        }
    }
}
