// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.reader.cursor.HoverCursor
import org.ppsspp.ppsspp.reader.hook.TextHookServer
import org.ppsspp.ppsspp.reader.mining.MediaCapture
import org.ppsspp.ppsspp.reader.model.ScreenLayout
import org.ppsspp.ppsspp.reader.model.TextLine
import org.ppsspp.ppsspp.reader.screen.ScreenText
import org.ppsspp.ppsspp.reader.screen.ScreenTexts
import org.ppsspp.ppsspp.reader.stats.ActivityTracker
import org.ppsspp.ppsspp.reader.stats.LineAccounting
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.ppsspp.ppsspp.reader.store.ReaderDatabase
import org.ppsspp.ppsspp.reader.ReaderLog as Log
import org.ppsspp.ppsspp.reader.ui.CrtSettings

/** A line after it was counted and stored in the backlog. */
data class PresentedLine(val line: TextLine, val accounting: LineAccounting, val backlogId: Long)

/** Whether a text profile is active for the running game. */
data class ProfileStatus(
    val titleId: String,
    val loaded: Boolean,
    val hooks: Int,
    val error: String
)

/**
 * The reading session of the running game: receives text from the core, counts it, keeps the
 * backlog and feeds everything that shows or forwards text.
 *
 * Listeners are called on the main thread. Database and counting work happens on one worker
 * thread, in arrival order.
 */
object ReaderSession {
    interface Listener {
        fun onLine(presented: PresentedLine) {}
        fun onProfileStatus(status: ProfileStatus) {}
        fun onActivityChanged() {}
        fun onCommand(command: Command) {}

        /** What [onScreen] returns changed: text appeared on screen, moved or went away. */
        fun onScreenTextChanged() {}

        /** [sessionActivity] changed. */
        fun onSessionActivity(counters: ReadingCounters?) {}
    }

    /** Actions that can be bound to controller buttons. */
    enum class Command { TOGGLE_PANEL, SENTENCE_CARD, BACKLOG }

    /** Sends [command] to whichever overlay holds the reader panel. Any thread. */
    fun dispatch(command: Command) {
        main.post { listeners.forEach { it.onCommand(command) } }
    }

    private const val DUPLICATE_WINDOW_MS = 1500L

    /** How long a button is held for the game, and how long a step of picking an answer takes. */
    private const val PRESS_MS = 80L
    private const val PICK_STEP_MS = 220L
    private const val MAX_PICK_PRESSES = 48
    private const val HEARTBEAT_MS = 1000L
    private const val FLUSH_INTERVAL_MS = 15_000L

    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<Listener>()
    private var worker: ExecutorService? = null

    private var appContext: Context? = null
    private var settings: ReaderSettings? = null
    private var database: ReaderDatabase? = null
    private var tracker: ActivityTracker? = null // worker thread only
    private var hookServer: TextHookServer? = null

    @Volatile
    private var titleId: String = ""
    private var title: String = ""

    @Volatile
    private var reading = false
    private var lastFlush = 0L // worker thread only
    private var lastLine: TextLine? = null // worker thread only
    private var lastPresented: PresentedLine? = null // worker thread only

    /** Which lines are on screen and where. Main thread. */
    internal val screen = ScreenTexts()

    /** The lines whose text the game is showing, with where each character is. Main thread. */
    val onScreen: Collection<ScreenText> get() = screen.onScreen

    /** Where [presented] is on screen, or null when its text is not shown (any more). */
    fun placementOf(presented: PresentedLine): ScreenText? = screen.placementOf(presented)

    /** Screenshots and audio for mining. Null while no game is running. */
    var media: MediaCapture? = null
        private set

    /** The line most recently presented. Main thread. */
    var current: PresentedLine? = null
        private set

    /**
     * What the reading session being added to holds so far: characters, time and the rest.
     * Null until a line was counted in this run. Main thread.
     */
    var sessionActivity: ReadingCounters? = null
        private set
    private var publishedActivity: ReadingCounters? = null // worker thread only

    /** Recent narrative lines, oldest first, to find where a line's audio ends. Main thread. */
    private val recent = ArrayDeque<PresentedLine>()
    private const val RECENT_LIMIT = 300

    /**
     * Audio history position where the line after [backlogId] began, or null when that line is
     * the newest one (its audio runs until now).
     */
    fun audioEndFor(backlogId: Long): Long? {
        val index = recent.indexOfFirst { it.backlogId == backlogId }
        if (index < 0 || index + 1 >= recent.size) return null
        return recent[index + 1].line.audioPosition.takeIf { it >= 0 }
    }

    /** A line of this run by its backlog id, with its audio position, or null. */
    fun recentLine(backlogId: Long): PresentedLine? = recent.firstOrNull { it.backlogId == backlogId }

    /** The latest choice list, shown beside the current line. Main thread. */
    var currentChoices: PresentedLine? = null
        private set

    var profileStatus: ProfileStatus = ProfileStatus("", false, 0, "")
        private set

    val isActive: Boolean get() = titleId.isNotEmpty()
    val gameKey: String get() = titleId
    val gameTitle: String get() = title

    fun database(context: Context): ReaderDatabase = synchronized(this) {
        database ?: ReaderDatabase(context.applicationContext).also { database = it }
    }

    fun addListener(listener: Listener) {
        listeners.addIfAbsent(listener)
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    /** Starts a session for the game being launched. Main thread. */
    fun start(context: Context, gameId: String, title: String) {
        stop()
        try {
            ReaderNative.attach()
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
        val app = context.applicationContext
        val prefs = ReaderSettings(app)
        val key = gameId.trim().uppercase()
        val db = database(app)
        appContext = app
        settings = prefs
        this.title = title
        this.titleId = key
        current = null
        currentChoices = null
        sessionActivity = null
        recent.clear()
        clearScreenTexts()
        profileStatus = ProfileStatus(key, false, 0, "")

        val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderWorker") }
        worker = executor
        executor.execute {
            db.setGameTitle(key, title)
            tracker = ActivityTracker(db, key, idleTimeoutMs = prefs.idleTimeoutMs).apply {
                paused = prefs.statsPaused
            }
            lastLine = null
            lastPresented = null
            publishedActivity = null
            lastFlush = System.currentTimeMillis()
        }

        media = MediaCapture(app, prefs)
        HoverCursor.configure(prefs.cursorButton, prefs.cursorSpeed, prefs.cursorToggle)
        setStatsButton(prefs)
        ReaderNative.setAudioCapture(prefs.captureAudio, prefs.audioHistorySeconds)
        CrtSettings.load(app)
        applyTextHookSettings()
        main.removeCallbacks(heartbeat)
        main.postDelayed(heartbeat, HEARTBEAT_MS)
    }

    /**
     * The core reports the running title. When the game was launched without a known title id
     * (opened from a file), the session started under a placeholder: restart it under the real
     * id, as long as nothing was read yet. Any thread.
     */
    internal fun adoptTitleId(key: String) {
        if (key == titleId || key.isEmpty()) return
        main.post {
            val context = appContext ?: return@post
            if (!isActive || current != null) return@post
            val keepReading = reading
            val keepFrames = media?.frameSource
            start(context, key, title)
            media?.frameSource = keepFrames
            setReading(keepReading)
        }
    }

    /** Ends the session and saves pending activity. Main thread. */
    fun stop() {
        if (!isActive) return
        main.removeCallbacks(heartbeat)
        val executor = worker
        worker = null
        executor?.execute {
            tracker?.flush()
            tracker = null
        }
        executor?.shutdown()
        stopping = executor
        appContext?.let { StatsSync.send(it) }
        hookServer?.stop()
        hookServer = null
        HoverCursor.cancel()
        try {
            ReaderInput.statsButton = -1
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
        media?.release()
        media = null
        ReaderNative.setAudioCapture(false, 0)
        titleId = ""
        reading = false
        current = null
        currentChoices = null
        clearScreenTexts()
        if (sessionActivity != null) {
            sessionActivity = null
            listeners.forEach { it.onSessionActivity(null) }
        }
    }

    /** The worker of the session that stopped last: it ends once the last activity is saved. */
    @Volatile
    private var stopping: java.util.concurrent.ExecutorService? = null

    /** Waits until what the last session counted is written. Not on the main thread. */
    fun awaitStopped(milliseconds: Long) {
        try {
            stopping?.awaitTermination(milliseconds, java.util.concurrent.TimeUnit.MILLISECONDS)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
    }

    /** True while the game is running in front of the user. Main thread. */
    fun setReading(value: Boolean) {
        if (reading == value) return
        reading = value
        if (!value) {
            // A paused game hears of no button being let go.
            HoverCursor.cancel()
            worker?.execute { tracker?.flush() }
        }
    }

    /** Re-reads preferences that affect a running session. Main thread. */
    fun onSettingsChanged() {
        val prefs = settings ?: return
        HoverCursor.configure(prefs.cursorButton, prefs.cursorSpeed, prefs.cursorToggle)
        setStatsButton(prefs)
        ReaderNative.setAudioCapture(prefs.captureAudio, prefs.audioHistorySeconds)
        applyTextHookSettings()
        worker?.execute {
            tracker?.paused = prefs.statsPaused
            tracker?.idleTimeoutMs = prefs.idleTimeoutMs
        }
        main.post { listeners.forEach { it.onActivityChanged() } }
    }

    /**
     * Pauses the reading statistics, or resumes them: the menu's entry and the statistics
     * button do this. Main thread.
     */
    fun toggleStatsPaused() {
        val prefs = settings ?: return
        prefs.statsPaused = !prefs.statsPaused
        onSettingsChanged()
    }

    /** The cursor's button wins when both are set to the same one. */
    private fun setStatsButton(prefs: ReaderSettings) {
        val button = prefs.statsButton.takeIf { it != prefs.cursorButton } ?: -1
        ReaderInput.statsButton = button
    }

    /** The user did something that shows they are still reading. Any thread. */
    fun noteInteraction() {
        val now = System.currentTimeMillis()
        worker?.execute { tracker?.onInteraction(now) }
    }

    /** Holds (true) or lets go of (false) one button of the console, by its number: the
     *  buttons a game played by touch still needs (ui/TouchButtons). Main thread. */
    fun holdButton(code: Int, down: Boolean) {
        try {
            ReaderNative.pressButton(code, down)
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
        if (down) noteInteraction()
    }

    /** Presses and releases the configured advance button in the game. Main thread. */
    fun pressAdvance() {
        press(settings?.advanceButton ?: ReaderInput.PSP_CIRCLE)
        noteInteraction()
    }

    /** Presses and releases one button of the PSP (a bit of its button word). */
    private fun press(bit: Int) {
        try {
            ReaderNative.pressButton(bit, true)
            main.postDelayed({
                try {
                    ReaderNative.pressButton(bit, false)
                } catch (_: UnsatisfiedLinkError) {
                }
            }, PRESS_MS)
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
    }

    private var picking: Runnable? = null

    /**
     * Picks answer [answer] (from 0) of the choice on screen as a player would with the
     * controller: moves the game's cursor there with the d-pad, one press at a time while the
     * profile reports where it is, then presses the advance button. False, and nothing pressed,
     * when the profile does not say where the cursor is. Main thread.
     */
    fun pickAnswer(answer: Int): Boolean {
        val first = try {
            ReaderNative.answerCursor()
        } catch (_: UnsatisfiedLinkError) {
            null
        } ?: return false
        if (first.isEmpty()) return false
        picking?.let { main.removeCallbacks(it) }
        var presses = 0
        var last = Int.MIN_VALUE
        var stuck = 0
        val step = object : Runnable {
            override fun run() {
                val cursor = try {
                    ReaderNative.answerCursor()
                } catch (_: UnsatisfiedLinkError) {
                    null
                }
                if (cursor == null || presses >= MAX_PICK_PRESSES) {
                    picking = null
                    return
                }
                val at = cursor[0]
                val sideways = cursor[1] == 1
                if (at == answer) {
                    picking = null
                    pressAdvance()
                    return
                }
                // A press that leaves the cursor where it was, twice: something else has the input (a
                // game's lesson over its first choice wants the confirm button first). Confirm once.
                stuck = if (at == last) stuck + 1 else 0
                last = at
                if (stuck >= 2) {
                    stuck = 0
                    last = Int.MIN_VALUE
                    pressAdvance()
                    presses++
                    main.postDelayed(this, PICK_STEP_MS)
                    return
                }
                press(
                    when {
                        at < answer -> if (sideways) ReaderInput.PSP_RIGHT else ReaderInput.PSP_DOWN
                        else -> if (sideways) ReaderInput.PSP_LEFT else ReaderInput.PSP_UP
                    }
                )
                presses++
                main.postDelayed(this, PICK_STEP_MS)
            }
        }
        picking = step
        step.run()
        noteInteraction()
        return true
    }

    /** Forgets the tracker's open session after sessions were deleted or cleared. */
    fun onActivityCleared() {
        worker?.execute {
            tracker?.reset()
            publishActivity()
        }
        main.post { listeners.forEach { it.onActivityChanged() } }
    }

    /** Saves pending activity now, then runs [then] on the main thread. */
    fun flush(then: (() -> Unit)? = null) {
        val executor = worker
        if (executor == null) {
            then?.let { main.post(it) }
            return
        }
        executor.execute {
            tracker?.flush()
            then?.let { main.post(it) }
        }
    }

    // ---- Called from the emulation thread ----

    internal fun onNativeEvent(json: String) {
        val now = System.currentTimeMillis()
        val line = TextLine.fromJson(json, now) ?: return
        val executor = worker ?: return
        try {
            executor.execute { handleLine(line, now) }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            // The session ended while the event was on its way.
        }
    }

    internal fun onNativeLayout(json: String) {
        val layout = ScreenLayout.fromJson(json) ?: return
        main.post { applyLayout(layout) }
    }

    internal fun onNativeLog(level: Int, message: String) {
        if (level >= 2) {
            main.post {
                profileStatus = profileStatus.copy(error = message)
                listeners.forEach { it.onProfileStatus(profileStatus) }
            }
        }
    }

    internal fun onNativeProfileStatus(loaded: Boolean, hooks: Int, message: String) {
        main.post {
            profileStatus = ProfileStatus(titleId, loaded, hooks, message)
            listeners.forEach { it.onProfileStatus(profileStatus) }
        }
    }

    // ---- Worker thread ----

    private fun handleLine(line: TextLine, now: Long) {
        val db = database ?: return
        val activity = tracker ?: return
        val previous = lastLine
        if (previous != null && previous.sourceId == line.sourceId &&
            previous.channel == line.channel && previous.text == line.text &&
            now - previous.timestamp < DUPLICATE_WINDOW_MS
        ) {
            // The game laid the same text out again. Not a new line to read, but layouts from
            // now on are reported for this event.
            val shown = lastPresented
            if (line.window.isNotEmpty() && shown != null) {
                val again = PresentedLine(line, shown.accounting, shown.backlogId)
                main.post { adoptWindowLine(again) }
            }
            return
        }
        lastLine = line
        val accounting = activity.onLine(line, now)
        publishActivity()
        val id = try {
            db.insertLine(titleId, line, accounting)
        } catch (e: Exception) {
            Log.error("[Reader] Could not store line: ${e.message}")
            -1L
        }
        if (now - lastFlush > FLUSH_INTERVAL_MS) {
            activity.flush()
            lastFlush = now
        }
        val presented = PresentedLine(line, accounting, id)
        lastPresented = presented
        main.post { present(presented) }
    }

    /** Passes the session's counters on when they changed. */
    private fun publishActivity() {
        val counters = tracker?.session
        if (counters == publishedActivity) return
        publishedActivity = counters
        val key = titleId
        main.post {
            if (titleId != key || !isActive) return@post
            sessionActivity = counters
            listeners.forEach { it.onSessionActivity(counters) }
        }
    }

    // ---- Main thread ----

    private fun present(presented: PresentedLine) {
        if (!isActive) return
        val line = presented.line
        if (line.isChoice) {
            currentChoices = presented
        } else {
            // The frame on screen still shows the previous line: keep it for that line.
            current?.let { media?.captureForLine(it.backlogId, immediately = true) }
            current = presented
            currentChoices = null
            recent.addLast(presented)
            while (recent.size > RECENT_LIMIT) recent.removeFirst()
            media?.captureForLine(presented.backlogId, immediately = false)
        }
        hookServer?.broadcast(line)
        if (settings?.clipboardAuto == true && !line.isChoice) copyToClipboard(line.text)
        listeners.forEach { it.onLine(presented) }
        if (line.window.isNotEmpty()) adoptWindowLine(presented)
    }

    /** [presented] is now the text of its window. */
    private fun adoptWindowLine(presented: PresentedLine) {
        if (isActive && screen.adoptLine(presented)) notifyScreenText()
    }

    private fun applyLayout(layout: ScreenLayout) {
        if (isActive && screen.applyLayout(layout)) notifyScreenText()
    }

    private fun clearScreenTexts() {
        if (screen.clear()) notifyScreenText()
    }

    private fun notifyScreenText() = listeners.forEach { it.onScreenTextChanged() }

    fun copyToClipboard(text: String) {
        val context = appContext ?: return
        try {
            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            clipboard.setPrimaryClip(ClipData.newPlainText("PPSSPP Reader", text))
        } catch (e: Exception) {
            Log.warning("[Reader] Clipboard refused the text: ${e.message}")
        }
    }

    private fun applyTextHookSettings() {
        val prefs = settings ?: return
        val running = hookServer
        val wanted = prefs.textHookEnabled
        if (running != null &&
            (!wanted || running.port != prefs.textHookPort || running.lan != prefs.textHookLan)
        ) {
            running.stop()
            hookServer = null
        }
        if (wanted && hookServer == null) {
            hookServer = TextHookServer(prefs.textHookPort, prefs.textHookLan).also { it.start() }
        }
        hookServer?.format = prefs.textHookFormat
        hookServer?.includeSpeaker = prefs.textHookSpeaker
        hookServer?.gameId = titleId
    }

    /** Address other programs can connect to, or null when the text server is off. */
    val textHookAddress: String? get() = hookServer?.describeAddress()

    private val heartbeat = object : Runnable {
        override fun run() {
            val now = System.currentTimeMillis()
            val isReading = reading
            current?.let { media?.keepAudioOf(it.backlogId, it.line.audioPosition) }
            worker?.execute {
                tracker?.onHeartbeat(now, isReading)
                publishActivity()
                if (now - lastFlush > FLUSH_INTERVAL_MS) {
                    tracker?.flush()
                    lastFlush = now
                    main.post { listeners.forEach { it.onActivityChanged() } }
                }
            }
            main.postDelayed(this, HEARTBEAT_MS)
        }
    }
}
