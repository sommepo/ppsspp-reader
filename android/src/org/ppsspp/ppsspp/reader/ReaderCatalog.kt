// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import java.io.File

/** What a text profile says of itself in its first lines. */
data class ProfileInfo(
    val titleId: String,
    /** The game's name; the title ID when the profile does not say. */
    val title: String,
    /** What of the game's text is read, in a line; may be empty. */
    val reads: String,
    /** The version of the game the profile was written for; may be empty. */
    val version: String,
    /** A profile the user put in the app's folder, which is used instead of a bundled one. */
    val own: Boolean,
    /**
     * What the profile says a card does with its line's audio ("never", "ask" or "always"), for
     * a game whose lines have no voices; empty when it says nothing. The user's own choice for
     * the game goes before it.
     */
    val audio: String = ""
)

/**
 * The games whose text can be read: those with a profile bundled with the app, and those the
 * user has put a profile of their own in the app's folder for.
 */
object ReaderCatalog {
    private val PROFILE_NAME = Regex("([A-Z]{4}[0-9]{5})\\.lua")

    /** Enough of a profile to hold the comments it begins with. */
    private const val HEAD_BYTES = 4096

    @Volatile
    private var known: Set<String>? = null

    @Volatile
    private var audioSaid: Map<String, String>? = null

    /**
     * Reads the lines "-- reader: <key> <value>" among the comments a profile begins with:
     * title, reads, version and audio. Other lines, and other keys, are passed over.
     */
    fun describe(titleId: String, source: String, own: Boolean): ProfileInfo {
        val said = HashMap<String, String>()
        for (raw in source.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            if (!line.startsWith("--")) break
            val text = line.removePrefix("--").trim()
            if (!text.startsWith("reader:")) continue
            val words = text.removePrefix("reader:").trim()
            val key = words.substringBefore(' ')
            val value = words.substringAfter(' ', "").trim()
            if (value.isNotEmpty()) said.putIfAbsent(key, value)
        }
        return ProfileInfo(
            titleId,
            said["title"] ?: titleId,
            said["reads"].orEmpty(),
            said["version"].orEmpty(),
            own,
            said["audio"].orEmpty().lowercase()
        )
    }

    /** The title ID a profile file of this name is for, or null when it is not a profile. */
    fun titleIdOf(fileName: String): String? = PROFILE_NAME.matchEntire(fileName)?.groupValues?.get(1)

    /** Every game with a profile, by title. The user's own profile of a game stands for it. */
    fun games(context: Context): List<ProfileInfo> {
        val games = LinkedHashMap<String, ProfileInfo>()
        for (file in userProfiles(context)) {
            val id = titleIdOf(file.name) ?: continue
            games[id] = describe(id, head(file.inputStream()), own = true)
        }
        for (id in bundledIds(context)) {
            if (id in games) continue
            val source = try {
                head(context.assets.open("${ReaderNative.PROFILE_ASSET_DIR}/$id.lua"))
            } catch (_: Exception) {
                ""
            }
            games[id] = describe(id, source, own = false)
        }
        known = games.keys.toSet()
        audioSaid = games.values.filter { it.audio.isNotEmpty() }.associate { it.titleId to it.audio }
        return games.values.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.title })
    }

    /** True when the game [titleId] (sixteen hexadecimal digits) has a profile. */
    fun covers(context: Context, titleId: String): Boolean {
        val ids = known ?: (bundledIds(context) + userProfiles(context).mapNotNull { titleIdOf(it.name) })
            .toSet().also { known = it }
        return titleId.uppercase() in ids
    }

    /** What the profile of the game [titleId] says of its lines' audio, or null. */
    fun cardAudio(context: Context, titleId: String): ReaderSettings.CardAudio? {
        if (audioSaid == null) games(context)
        return ReaderSettings.CardAudio.of(audioSaid?.get(titleId.uppercase()))
    }

    /** Looks again at the next question: for tests, and after a profile was added or removed. */
    fun forget() {
        known = null
        audioSaid = null
    }

    private fun bundledIds(context: Context): List<String> = try {
        context.assets.list(ReaderNative.PROFILE_ASSET_DIR).orEmpty().mapNotNull(::titleIdOf)
    } catch (_: Exception) {
        emptyList()
    }

    private fun userProfiles(context: Context): List<File> = try {
        context.getExternalFilesDir(null)
            ?.let { File(it, ReaderNative.PROFILE_USER_DIR).listFiles() }
            ?.filter { it.isFile }
            .orEmpty()
    } catch (_: Exception) {
        emptyList()
    }

    private fun head(stream: java.io.InputStream): String = stream.use {
        val bytes = ByteArray(HEAD_BYTES)
        var read = 0
        while (read < bytes.size) {
            val n = it.read(bytes, read, bytes.size - read)
            if (n < 0) break
            read += n
        }
        // A character cut in two at the end is in no line that matters: those end with a line break.
        String(bytes, 0, read, Charsets.UTF_8)
    }
}
