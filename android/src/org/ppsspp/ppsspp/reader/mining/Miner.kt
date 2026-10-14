// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.PresentedLine
import org.ppsspp.ppsspp.reader.ReaderCatalog
import org.ppsspp.ppsspp.reader.ReaderSession
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.anki.AnkiClient
import org.ppsspp.ppsspp.reader.anki.AnkiConnectClient
import org.ppsspp.ppsspp.reader.anki.AnkiDroidClient
import org.ppsspp.ppsspp.reader.anki.AnkiException
import org.ppsspp.ppsspp.reader.anki.CardBuilder
import org.ppsspp.ppsspp.reader.anki.CardContext
import org.ppsspp.ppsspp.reader.anki.Duplicates
import org.ppsspp.ppsspp.reader.anki.SourceAddress
import org.ppsspp.ppsspp.reader.dict.LookupResult
import org.ppsspp.ppsspp.reader.ReaderLog as Log

/**
 * Makes an Anki card from a line: gathers the screenshot and audio the note's fields ask for,
 * stores them in Anki, then adds the note. Reading statistics and the game are not affected.
 */
class Miner(context: Context) {
    sealed class Outcome {
        data class Added(val noteId: Long) : Outcome()

        /** Nothing was added: Anki has the word already, as [notes]. */
        data class Exists(val notes: List<Long>) : Outcome()
        data class Failed(val message: String) : Outcome()

        /** Nothing was added: the user was asked about the audio and called the card off. */
        object Cancelled : Outcome()
    }

    /**
     * Puts the question of a game whose cards ask about their audio: does this card get [clip],
     * what the game played during its line? [answer] takes true to keep it, false to make the
     * card without it, null to make no card. Main thread.
     */
    fun interface AudioQuestion {
        fun ask(clip: File, answer: (keep: Boolean?) -> Unit)
    }

    private val context = context.applicationContext
    private val settings = ReaderSettings(this.context)
    private val main = Handler(Looper.getMainLooper())

    /**
     * The configured client, or null with the reason it cannot be used. A [quick] one gives up
     * on an Anki that does not answer soon: for questions the user is not waiting on.
     */
    fun client(quick: Boolean = false): Pair<AnkiClient?, String?> = when (settings.ankiTransport) {
        ReaderSettings.ANKI_ANKIDROID -> when {
            !AnkiDroidClient.isInstalled(context) ->
                null to context.getString(R.string.reader_ankidroid_missing)

            !AnkiDroidClient.hasPermission(context) ->
                null to context.getString(R.string.reader_ankidroid_permission)

            else -> AnkiDroidClient(context) to null
        }

        ReaderSettings.ANKI_ANKICONNECT -> if (settings.ankiConnectUrl.isBlank()) {
            null to context.getString(R.string.reader_anki_not_configured)
        } else if (quick) {
            AnkiConnectClient(
                settings.ankiConnectUrl.trim(),
                settings.ankiConnectKey,
                QUICK_CONNECT_MS,
                QUICK_READ_MS
            ) to null
        } else {
            AnkiConnectClient(settings.ankiConnectUrl.trim(), settings.ankiConnectKey) to null
        }

        else -> null to context.getString(R.string.reader_anki_disabled)
    }

    /**
     * Adds a card for [presented], about [word] when one was looked up. Whether it gets its
     * line's audio is the game's setting; where that is to ask, [question] is asked, and a card
     * made where nobody can be asked gets the audio. [callback] runs on the main thread. Call
     * from the main thread.
     */
    fun mine(
        presented: PresentedLine,
        word: LookupResult?,
        wordRange: IntRange?,
        question: AudioQuestion? = null,
        callback: (Outcome) -> Unit
    ) {
        val (client, problem) = client()
        val templates = CardBuilder.parseMapping(settings.ankiFieldMap).filterValues { it.isNotBlank() }
        if (client == null) {
            callback(Outcome.Failed(problem ?: ""))
            return
        }
        if (settings.ankiDeck.isBlank() || settings.ankiModel.isBlank() || templates.isEmpty()) {
            callback(Outcome.Failed(context.getString(R.string.reader_anki_not_configured)))
            return
        }

        val media = mediaOverride ?: ReaderSession.media
        val isCurrent = ReaderSession.current?.backlogId == presented.backlogId
        val audio = settings.cardAudioFor(
            ReaderSession.gameKey,
            ReaderCatalog.cardAudio(context, ReaderSession.gameKey)
        )
        val wantsPicture = CardBuilder.needsScreenshot(templates.values) && media != null
        val wantsAudio = audio != ReaderSettings.CardAudio.NEVER &&
            CardBuilder.needsAudio(templates.values) && media != null &&
            presented.line.audioPosition >= 0

        // Gather media (each step may finish asynchronously), then hand over to the worker.
        fun withAudio(picture: File?) {
            if (!wantsAudio) {
                send(client, templates, presented, word, wordRange, picture, null, callback)
                return
            }
            media!!.audioClip(
                presented.backlogId,
                presented.line.audioPosition,
                ReaderSession.audioEndFor(presented.backlogId)
            ) { clip ->
                if (clip == null || audio != ReaderSettings.CardAudio.ASK || question == null) {
                    send(client, templates, presented, word, wordRange, picture, clip, callback)
                    return@audioClip
                }
                question.ask(clip) { keep ->
                    when (keep) {
                        true -> send(client, templates, presented, word, wordRange, picture, clip, callback)

                        false -> {
                            clip.delete()
                            send(client, templates, presented, word, wordRange, picture, null, callback)
                        }

                        null -> {
                            discard(picture, clip)
                            callback(Outcome.Cancelled)
                        }
                    }
                }
            }
        }
        when {
            !wantsPicture -> withAudio(null)

            // The scene on screen belongs to the current line; older lines use what was kept.
            isCurrent -> media!!.captureNow { withAudio(it ?: media.screenshotFor(presented.backlogId)) }

            else -> withAudio(media!!.screenshotFor(presented.backlogId))
        }
    }

    private fun send(
        client: AnkiClient,
        templates: Map<String, String>,
        presented: PresentedLine,
        word: LookupResult?,
        wordRange: IntRange?,
        picture: File?,
        clip: File?,
        callback: (Outcome) -> Unit
    ) {
        val deck = settings.ankiDeck
        val model = settings.ankiModel
        val tags = settings.ankiTags.split(' ').filter { it.isNotBlank() }
        val duplicates = when {
            settings.ankiAllowDuplicates -> Duplicates.ALLOW
            settings.ankiDuplicatesAnywhere -> Duplicates.REFUSE_ANYWHERE
            else -> Duplicates.REFUSE_IN_DECK
        }
        val scope = scopeDeck()
        val gameTitle = ReaderSession.gameTitle
        val gameUrl = SourceAddress.of(ReaderSession.gameKey)
        // The field VN Library's media add-on asked an address for, if cards go through it.
        val addressField = (client as? AnkiConnectClient)?.let { settings.ankiAddressField(settings.ankiConnectUrl) }
        worker.execute {
            val outcome = try {
                val card = CardContext(presented.line, gameTitle, word, wordRange, gameUrl = gameUrl)
                // Not every Anki refuses a duplicate when asked to (AnkiConnect Android and
                // AnkiDroid add whatever they are given), so look before sending anything.
                val key = if (duplicates == Duplicates.ALLOW) null else keyOf(client, model, templates, card)
                val existing = key?.let { client.findNotes(scope, it.first, it.second) }.orEmpty()
                if (existing.isNotEmpty()) {
                    remember(key!!, scope, existing)
                    Outcome.Exists(existing)
                } else {
                    val stamp = System.currentTimeMillis()
                    val pictureName = picture?.let { client.storeMedia(it, "ppsspp_reader_$stamp.jpg") }
                    val audioName = clip?.let { client.storeMedia(it, "ppsspp_reader_$stamp.m4a") }
                    // The word as somebody says it, when a field asks for it and the card has a
                    // word: fetched now, and left out when it cannot be had.
                    val wordAudioName = word?.takeIf { CardBuilder.needsWordAudio(templates.values) }?.let {
                        val file = File.createTempFile("word", ".mp3")
                        try {
                            if (WordAudio.fetch(it.expression, it.reading, file)) {
                                client.storeMedia(file, WordAudio.fileName(it.expression, it.reading))
                            } else {
                                null
                            }
                        } catch (e: Exception) {
                            null
                        } finally {
                            file.delete()
                        }
                    }
                    val fields = CardBuilder.build(
                        templates,
                        card.copy(screenshotName = pictureName, audioName = audioName, wordAudioName = wordAudioName)
                    )
                    val added = SourceAddress.addNote(
                        client, deck, model, fields, tags, duplicates, gameUrl, gameTitle, addressField
                    )
                    if (added.field != null && added.field != addressField) {
                        settings.rememberAnkiAddressField(settings.ankiConnectUrl, added.field)
                    }
                    val id = added.id
                    (key ?: runCatching { keyOf(client, model, templates, card) }.getOrNull())
                        ?.let { remember(it, scope, listOf(id)) }
                    Outcome.Added(id)
                }
            } catch (e: AnkiException) {
                Outcome.Failed(e.message ?: "Anki error")
            } catch (e: Exception) {
                Log.error("[Reader] Card failed: $e")
                Outcome.Failed(e.message ?: e.javaClass.simpleName)
            } finally {
                discard(picture, clip)
            }
            main.post { callback(outcome) }
        }
    }

    /** Removes what was gathered for one card. */
    private fun discard(picture: File?, clip: File?) {
        clip?.delete()
        // Kept line screenshots stay for other cards; one-off captures are removed.
        if (picture != null && picture.name.startsWith("now-")) picture.delete()
    }

    // ---- What Anki already has ----

    /**
     * Asks Anki which notes it has for [word] already, the way a card made from it now would be
     * recognised: by the note type's first field, in the deck cards go to (or anywhere, when the
     * user chose that). [callback] gets their ids on the main thread, an empty list when there
     * are none, or null when that cannot be told. Call from the main thread.
     */
    fun findExisting(
        presented: PresentedLine,
        word: LookupResult,
        wordRange: IntRange?,
        callback: (List<Long>?) -> Unit
    ) {
        val (client, _) = client(quick = true)
        val model = settings.ankiModel
        val templates = CardBuilder.parseMapping(settings.ankiFieldMap).filterValues { it.isNotBlank() }
        if (client == null || settings.ankiDeck.isBlank() || model.isBlank() || templates.isEmpty()) {
            callback(null)
            return
        }
        val scope = scopeDeck()
        val card = CardContext(
            presented.line, ReaderSession.gameTitle, word, wordRange,
            gameUrl = SourceAddress.of(ReaderSession.gameKey)
        )
        checker.execute {
            val found = try {
                if (System.currentTimeMillis() < quietUntil) {
                    null
                } else {
                    keyOf(client, model, templates, card)?.let { key ->
                        recalled(key, scope) ?: client.findNotes(scope, key.first, key.second)
                            .also { remember(key, scope, it) }
                    }
                }
            } catch (e: Exception) {
                // An Anki that is not there is asked again later, not for every word tapped.
                quietUntil = System.currentTimeMillis() + QUIET_AFTER_FAILURE_MS
                Log.warning("[Reader] Could not ask Anki about a word: ${e.message}")
                null
            }
            main.post { callback(found) }
        }
    }

    /** Opens Anki's browser on [notes]; [callback] gets null, or what went wrong. Main thread. */
    fun showNotes(notes: List<Long>, callback: (String?) -> Unit) {
        val (client, problem) = client()
        if (client == null) {
            callback(problem ?: "")
            return
        }
        worker.execute {
            val failure = try {
                client.showNotes(notes)
                null
            } catch (e: Exception) {
                e.message ?: e.javaClass.simpleName
            }
            main.post { callback(failure) }
        }
    }

    /** The deck existing notes are looked for in, or null for the whole collection. */
    private fun scopeDeck(): String? = if (settings.ankiDuplicatesAnywhere) null else settings.ankiDeck

    /**
     * The field Anki tells notes apart by (the note type's first) and what a card made from
     * [card] would put in it, or null when the mapping leaves that field empty.
     */
    private fun keyOf(
        client: AnkiClient,
        model: String,
        templates: Map<String, String>,
        card: CardContext
    ): Pair<String, String>? {
        // Recorded when the setup was saved. For a setup saved before that was recorded, Anki
        // is asked once and the answer kept for as long as the app runs.
        val field = settings.ankiKeyField(model)
            ?: synchronized(keyFields) { keyFields[model] }
            ?: client.fieldNames(model).firstOrNull()?.also { synchronized(keyFields) { keyFields[model] = it } }
            ?: return null
        val template = templates[field] ?: return null
        val value = CardBuilder.build(mapOf(field to template), card)[field].orEmpty()
        return if (value.isBlank()) null else field to value
    }

    private fun cacheKey(key: Pair<String, String>, scope: String?) =
        listOf(settings.ankiTransport, settings.ankiConnectUrl, scope ?: "\u0000", key.first, key.second)
            .joinToString("\u001f")

    private fun recalled(key: Pair<String, String>, scope: String?): List<Long>? = synchronized(known) {
        known[cacheKey(key, scope)]?.takeIf { System.currentTimeMillis() - it.first < KNOWN_FOR_MS }?.second
    }

    private fun remember(key: Pair<String, String>, scope: String?, notes: List<Long>) = synchronized(known) {
        if (known.size > KNOWN_LIMIT) known.clear()
        known[cacheKey(key, scope)] = System.currentTimeMillis() to notes
    }

    companion object {
        private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderAnki") }

        // Questions about what Anki has do not wait behind a card that is being sent.
        private val checker = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderAnkiCheck") }

        /** What Anki said it has, for a short while: the same word is often tapped twice. */
        private val known = HashMap<String, Pair<Long, List<Long>>>()

        /** First fields of note types, as Anki named them. */
        private val keyFields = HashMap<String, String>()

        @Volatile
        private var quietUntil = 0L

        /** Stands in for the running game's pictures and audio, where there is no emulator. */
        @Volatile
        internal var mediaOverride: CardMedia? = null

        private const val KNOWN_FOR_MS = 60_000L
        private const val KNOWN_LIMIT = 500
        private const val QUIET_AFTER_FAILURE_MS = 20_000L
        private const val QUICK_CONNECT_MS = 1_500
        private const val QUICK_READ_MS = 5_000

        /** Forgets what was learned from Anki, for tests and after the setup changed. */
        fun forget() {
            synchronized(known) { known.clear() }
            synchronized(keyFields) { keyFields.clear() }
            quietUntil = 0L
        }
    }
}
