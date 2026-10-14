// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest

/**
 * The word of a card as somebody says it: a recording from JapanesePod101's dictionary, the
 * same place Yomitan asks first. The address takes the word as written and its reading; it
 * answers a word it has with the recording (by way of another address) and a word it has not
 * with one and the same recording of a voice saying so, which is told by its fingerprint and
 * never put on a card.
 *
 * Asked only when a card's fields use {word-audio}; the word and its reading go to that
 * server and nothing else does.
 */
object WordAudio {
    private const val ADDRESS = "https://assets.languagepod101.com/dictionary/japanese/audiomp3.php"

    /** SHA-256 of the recording that says the dictionary has no such word. */
    const val MISSING = "ae6398b5a27bc8c0a771df6c907ade794be15518174773c58c7c7ddd17098906"
    private const val LARGEST = 2 * 1024 * 1024
    private const val CONNECT_TIMEOUT_MS = 5_000
    private const val READ_TIMEOUT_MS = 8_000

    /** Fetches what an address answers, following where it points to; null when it cannot be had. */
    var fetcher: (String) -> ByteArray? = ::download

    /**
     * Where the recording of [expression] read as [reading] is asked for. A word written in
     * kana alone is asked for by its reading only, as the dictionary lists it.
     */
    fun address(expression: String, reading: String): String {
        val kana = reading.ifEmpty { expression }
        val written = if (expression == kana || expression.isEmpty()) "" else expression
        val parts = ArrayList<String>()
        if (written.isNotEmpty()) parts += "kanji=" + URLEncoder.encode(written, "UTF-8")
        if (kana.isNotEmpty()) parts += "kana=" + URLEncoder.encode(kana, "UTF-8")
        return ADDRESS + "?" + parts.joinToString("&")
    }

    /** The name the recording is kept under in Anki: the same word gets the same file. */
    fun fileName(expression: String, reading: String): String {
        val digest = sha256("$expression\u0000$reading".toByteArray(Charsets.UTF_8)).take(16)
        return "reader_word_$digest.mp3"
    }

    /** True when [bytes] are a recording of a word: something, and not the "no such word" one. */
    fun isRecording(bytes: ByteArray?): Boolean =
        bytes != null && bytes.size in 256..LARGEST && sha256(bytes) != MISSING

    /**
     * Writes the recording of the word into [into]. False when the dictionary has none or
     * cannot be reached: a card is then made without it.
     */
    fun fetch(expression: String, reading: String, into: File): Boolean {
        if (expression.isBlank() && reading.isBlank()) return false
        val bytes = try {
            fetcher(address(expression, reading))
        } catch (_: Exception) {
            null
        }
        if (!isRecording(bytes)) return false
        into.writeBytes(bytes!!)
        return true
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    private fun download(address: String): ByteArray? {
        var at = address
        repeat(4) {
            val connection = URL(at).openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = CONNECT_TIMEOUT_MS
                connection.readTimeout = READ_TIMEOUT_MS
                connection.instanceFollowRedirects = false
                val status = connection.responseCode
                if (status in 300..399) {
                    at = URL(URL(at), connection.getHeaderField("Location") ?: return null).toString()
                    if (!at.startsWith("https://")) return null
                    return@repeat
                }
                if (status != 200) return null
                return connection.inputStream.use { input ->
                    val out = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(16 * 1024)
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        out.write(buffer, 0, n)
                        if (out.size() > LARGEST) return null
                    }
                    out.toByteArray()
                }
            } finally {
                connection.disconnect()
            }
        }
        return null
    }
}
