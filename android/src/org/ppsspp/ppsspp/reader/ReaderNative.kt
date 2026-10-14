// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.annotation.Keep
import java.io.File
import java.io.IOException
import org.ppsspp.ppsspp.reader.ReaderLog as Log

/**
 * The bridge to the text engine in the emulator's native library (android/jni/reader_jni.cpp).
 * The functions marked external are answered there; the others are called from there, on a
 * thread of the bridge's own.
 */
object ReaderNative {
    /** Tells the native side where to send what the engine reports. Once is enough. */
    external fun attach()

    /** Loads the running game's profile again (after it was edited). */
    external fun reloadProfile()

    /** Keeps the last [seconds] of the game's sound, for a card's audio. */
    external fun setAudioCapture(enabled: Boolean, seconds: Int)

    external fun audioPosition(): Long

    external fun audioSampleRate(): Int

    external fun readAudio(start: Long, end: Long, actualStart: LongArray): ShortArray?

    /**
     * The surface's width and height in pixels, then the left, top, right and bottom of the
     * game's picture on it. Null while no game is running.
     */
    external fun pictureRect(): IntArray?

    /** While held, the game sees its stick at rest: the reader's cursor has the sticks. */
    external fun holdSticks(held: Boolean)

    /**
     * The address and the code of save sync (the emulator's own page, Reader, Save sync) while
     * it is switched on and set up; null otherwise.
     */
    external fun saveSyncService(): Array<String>?

    /** Presses or releases a button of the PSP, by the number of its bit in the button word. */
    external fun pressButton(bit: Int, down: Boolean)

    /** True while the game itself is in front: not the emulator's menus, not its pause screen. */
    external fun inGame(): Boolean

    internal const val PROFILE_ASSET_DIR = "reader/profiles"
    internal const val PROFILE_USER_DIR = "reader/profiles"

    private val main = Handler(Looper.getMainLooper())

    /** The application, for the profiles in its assets and in its folder. */
    @Volatile
    var appContext: Context? = null

    /** What is told when a game starts or stops, and when the emulator's menu asks for the reader. */
    interface Frontend {
        fun onGameStarted(gameId: String, title: String)
        fun onGameStopped()
        fun onMenu(what: String)
    }

    @Volatile
    var frontend: Frontend? = null

    @Volatile
    private var currentGameId: String = ""

    private var attached = false

    /** [attach], once, and without failing where there is no native library (tests). */
    fun attachOnce() {
        if (attached) return
        try {
            attach()
            attached = true
        } catch (_: UnsatisfiedLinkError) {
            // No emulator behind this (off-device tests).
        }
    }

    // ---- Called from native code ----

    @Keep
    @JvmStatic
    fun onTextEvent(json: ByteArray) {
        ReaderSession.onNativeEvent(String(json, Charsets.UTF_8))
    }

    @Keep
    @JvmStatic
    fun onLayout(json: ByteArray) {
        ReaderSession.onNativeLayout(String(json, Charsets.UTF_8))
    }

    @Keep
    @JvmStatic
    fun onLog(level: Int, message: ByteArray) {
        val text = String(message, Charsets.UTF_8)
        when (level) {
            0 -> Log.info("[Reader] $text")
            1 -> Log.warning("[Reader] $text")
            else -> Log.error("[Reader] $text")
        }
        ReaderSession.onNativeLog(level, text)
    }

    @Keep
    @JvmStatic
    fun onProfileStatus(loaded: Boolean, hooks: Int, message: ByteArray) {
        ReaderSession.onNativeProfileStatus(loaded, hooks, String(message, Charsets.UTF_8))
    }

    /** A game is loaded: its ID as its disc gives it ("ULJS00219") and its name. */
    @Keep
    @JvmStatic
    fun onGameStarted(gameId: String, title: ByteArray) {
        currentGameId = gameId
        val name = String(title, Charsets.UTF_8)
        main.post { frontend?.onGameStarted(gameId, name) }
    }

    @Keep
    @JvmStatic
    fun onGameStopped() {
        main.post { frontend?.onGameStopped() }
    }

    /** The emulator's own menu asks for one of the reader's screens. */
    @Keep
    @JvmStatic
    fun onMenu(what: ByteArray) {
        val name = String(what, Charsets.UTF_8)
        main.post { frontend?.onMenu(name) }
    }

    /**
     * The profile for a game: a file of the user's wins over the bundled one. Null when there
     * is none.
     */
    @Keep
    @JvmStatic
    fun findProfile(gameId: String): ByteArray? {
        currentGameId = gameId
        return profileSource(gameId)
    }

    @Keep
    @JvmStatic
    fun profileSource(gameId: String): ByteArray? =
        readUserFile("$gameId.lua") ?: readAsset("$gameId.lua")

    /** A data file a profile asks for with reader.data(name): `<GAMEID>.name`. */
    @Keep
    @JvmStatic
    fun loadProfileData(name: String): ByteArray? {
        if (name.contains("..") || name.contains('/') || name.contains('\\')) return null
        val scoped = "$currentGameId.$name"
        return readUserFile(scoped) ?: readAsset(scoped)
    }

    fun hasUserProfile(gameId: String): Boolean = userProfileFile("$gameId.lua") != null

    fun hasBundledProfile(gameId: String): Boolean = try {
        appContext?.assets?.open("$PROFILE_ASSET_DIR/$gameId.lua")?.close() != null
    } catch (_: IOException) {
        false
    }

    private fun readAsset(name: String): ByteArray? = try {
        appContext?.assets?.open("$PROFILE_ASSET_DIR/$name")?.use { it.readBytes() }
    } catch (_: IOException) {
        null
    }

    private fun userProfileFile(name: String): File? = try {
        val root = appContext?.getExternalFilesDir(null)
        root?.let { File(File(it, PROFILE_USER_DIR), name) }?.takeIf { it.isFile }
    } catch (e: Exception) {
        Log.warning("[Reader] Cannot look for $name in the app's folder: ${e.message}")
        null
    }

    private fun readUserFile(name: String): ByteArray? {
        val file = userProfileFile(name) ?: return null
        return try {
            file.readBytes()
        } catch (e: Exception) {
            Log.warning("[Reader] Cannot read $file: ${e.message}")
            null
        }
    }
}
