// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.anki

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.util.Base64
import org.json.JSONArray
import org.json.JSONObject

/**
 * AnkiConnect (version 6 protocol): Anki on a computer reached over the network, or AnkiConnect
 * Android on this device. Media is sent inline, so the other side does not need access to this
 * device. Only actions that AnkiConnect Android has as well are used.
 *
 * AnkiConnect speaks plain HTTP, and Android refuses plain HTTP through its own HTTP client
 * unless the whole app is allowed it. Rather than lift that for everything in the app, the
 * request to the one address the user entered is written to a socket here. An https address
 * (AnkiConnect behind a proxy) goes through the platform's client as usual.
 */
class AnkiConnectClient(
    private val url: String,
    private val key: String = "",
    private val connectTimeoutMs: Int = CONNECT_TIMEOUT_MS,
    private val readTimeoutMs: Int = READ_TIMEOUT_MS
) : AnkiClient {

    override fun deckNames(): List<String> = strings(invoke("deckNames"))

    override fun modelNames(): List<String> = strings(invoke("modelNames"))

    override fun fieldNames(model: String): List<String> =
        strings(invoke("modelFieldNames", JSONObject().put("modelName", model)))

    override fun storeMedia(file: File, preferredName: String): String {
        val data = Base64.getEncoder().encodeToString(file.readBytes())
        val result = invoke(
            "storeMediaFile",
            JSONObject().put("filename", preferredName).put("data", data)
        )
        return (result as? String)?.takeIf { it.isNotEmpty() } ?: preferredName
    }

    override fun addNote(
        deck: String,
        model: String,
        fields: Map<String, String>,
        tags: List<String>,
        duplicates: Duplicates
    ): Long {
        val options = JSONObject().put("allowDuplicate", duplicates == Duplicates.ALLOW)
        if (duplicates == Duplicates.REFUSE_IN_DECK) {
            options.put("duplicateScope", "deck").put(
                "duplicateScopeOptions",
                JSONObject().put("deckName", deck).put("checkChildren", true)
            )
        }
        val note = JSONObject()
            .put("deckName", deck)
            .put("modelName", model)
            .put("fields", JSONObject(fields as Map<*, *>))
            .put("tags", JSONArray(tags))
            .put("options", options)
        val result = invoke("addNote", JSONObject().put("note", note))
        return (result as? Number)?.toLong()
            ?: throw AnkiException("Anki did not create the note")
    }

    override fun findNotes(deck: String?, field: String, value: String): List<Long> {
        val query = AnkiSearch.fieldIs(deck, field, value)
        val array = invoke("findNotes", JSONObject().put("query", query)) as? JSONArray
            ?: throw AnkiException("Unexpected reply from AnkiConnect")
        return List(array.length()) { array.optLong(it) }
    }

    override fun showNotes(noteIds: List<Long>) {
        if (noteIds.isEmpty()) return
        invoke("guiBrowse", JSONObject().put("query", AnkiSearch.notes(noteIds)))
    }

    /** The protocol version the server reports; a cheap way to test the connection. */
    fun version(): Int = (invoke("version") as? Number)?.toInt()
        ?: throw AnkiException("Unexpected reply from AnkiConnect")

    private fun strings(value: Any?): List<String> {
        val array = value as? JSONArray ?: throw AnkiException("Unexpected reply from AnkiConnect")
        return List(array.length()) { array.optString(it) }
    }

    private fun invoke(action: String, params: JSONObject? = null): Any? {
        val request = JSONObject().put("action", action).put("version", 6)
        if (params != null) request.put("params", params)
        if (key.isNotEmpty()) request.put("key", key)
        val body = request.toString().toByteArray(Charsets.UTF_8)

        val text = try {
            // "192.168.1.5:8765" is what people type; that is http.
            val address = url.trim()
            if (address.isEmpty()) {
                throw AnkiException("Enter the address of AnkiConnect, for example http://192.168.1.5:8765")
            }
            val target = URI(if ("://" in address) address else "http://$address")
            when (target.scheme?.lowercase()) {
                "http" -> postPlain(target, body)
                "https" -> postSecure(target, body)
                else -> throw AnkiException("The AnkiConnect address must start with http:// or https://")
            }
        } catch (e: AnkiException) {
            throw e
        } catch (e: Exception) {
            throw AnkiException("Cannot reach AnkiConnect at $url: ${e.message}", e)
        }

        val reply = try {
            JSONObject(text)
        } catch (e: Exception) {
            throw AnkiException("AnkiConnect sent something that is not JSON", e)
        }
        if (!reply.isNull("error")) throw AnkiException(reply.optString("error"))
        return if (reply.isNull("result")) null else reply.get("result")
    }

    private fun postSecure(target: URI, body: ByteArray): String {
        val connection = target.toURL().openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = connectTimeoutMs
            connection.readTimeout = readTimeoutMs
            connection.requestMethod = "POST"
            connection.doOutput = true
            connection.instanceFollowRedirects = false
            connection.setRequestProperty("Content-Type", "application/json")
            connection.outputStream.use { it.write(body) }
            val status = connection.responseCode
            if (status != HttpURLConnection.HTTP_OK) throw AnkiException("AnkiConnect answered HTTP $status")
            return connection.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        } finally {
            connection.disconnect()
        }
    }

    /** One HTTP/1.1 POST over a plain socket; the reply's body as text. */
    private fun postPlain(target: URI, body: ByteArray): String {
        val host = target.host ?: throw AnkiException("The AnkiConnect address has no host name")
        val port = if (target.port > 0) target.port else 80
        val path = buildString {
            append(target.rawPath?.takeIf { it.isNotEmpty() } ?: "/")
            target.rawQuery?.let { append('?').append(it) }
        }
        Socket().use { socket ->
            socket.connect(InetSocketAddress(host, port), connectTimeoutMs)
            socket.soTimeout = readTimeoutMs
            val out = socket.getOutputStream()
            val hostHeader = if (host.contains(':')) "[$host]" else host
            out.write(
                (
                    "POST $path HTTP/1.1\r\nHost: $hostHeader:$port\r\n" +
                        "Content-Type: application/json\r\nContent-Length: ${body.size}\r\n" +
                        "Connection: close\r\n\r\n"
                    ).toByteArray(Charsets.US_ASCII)
            )
            out.write(body)
            out.flush()

            val input = BufferedInputStream(socket.getInputStream())
            val statusLine = readLine(input) ?: throw AnkiException("AnkiConnect closed the connection")
            val status = statusLine.split(' ').getOrNull(1)?.toIntOrNull()
                ?: throw AnkiException("AnkiConnect sent something that is not HTTP")
            var length = -1
            var chunked = false
            while (true) {
                val line = readLine(input) ?: break
                if (line.isEmpty()) break
                val colon = line.indexOf(':')
                if (colon <= 0) continue
                val name = line.substring(0, colon).trim().lowercase()
                val value = line.substring(colon + 1).trim()
                when (name) {
                    "content-length" -> length = value.toIntOrNull() ?: -1
                    "transfer-encoding" -> chunked = value.lowercase().contains("chunked")
                }
            }
            if (status != HttpURLConnection.HTTP_OK) throw AnkiException("AnkiConnect answered HTTP $status")
            val bytes = when {
                chunked -> readChunked(input)
                length >= 0 -> readExactly(input, length.coerceAtMost(MAX_REPLY_BYTES))
                else -> input.readBytes()
            }
            return bytes.toString(Charsets.UTF_8)
        }
    }

    /** A header line without its line break, or null at the end of the stream. */
    private fun readLine(input: InputStream): String? {
        val line = ByteArrayOutputStream()
        while (true) {
            val c = input.read()
            if (c < 0) return if (line.size() == 0) null else line.toString("ISO-8859-1")
            if (c == '\n'.code) return line.toString("ISO-8859-1").removeSuffix("\r")
            line.write(c)
            if (line.size() > MAX_HEADER_BYTES) throw AnkiException("AnkiConnect sent an overlong header")
        }
    }

    private fun readExactly(input: InputStream, count: Int): ByteArray {
        val bytes = ByteArray(count)
        var read = 0
        while (read < count) {
            val n = input.read(bytes, read, count - read)
            if (n < 0) break
            read += n
        }
        return if (read == count) bytes else bytes.copyOf(read)
    }

    private fun readChunked(input: InputStream): ByteArray {
        val out = ByteArrayOutputStream()
        while (true) {
            val size = readLine(input)?.substringBefore(';')?.trim()?.toIntOrNull(16) ?: break
            if (size <= 0) break
            if (out.size() + size > MAX_REPLY_BYTES) throw AnkiException("AnkiConnect's reply is too large")
            out.write(readExactly(input, size))
            readLine(input) // the line break after the chunk
        }
        return out.toByteArray()
    }

    companion object {
        const val CONNECT_TIMEOUT_MS = 8_000
        const val READ_TIMEOUT_MS = 30_000
        private const val MAX_HEADER_BYTES = 16 * 1024
        private const val MAX_REPLY_BYTES = 8 * 1024 * 1024
    }
}
