// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import android.content.ContextWrapper
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Typeface
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.test.core.app.ApplicationProvider
import java.io.BufferedInputStream
import java.io.File
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import org.json.JSONArray
import org.json.JSONObject
import org.robolectric.Shadows.shadowOf

/** Helpers shared by the reader's Robolectric tests. */
object TestSupport {
    /** Gives the reader its application, as the emulator's activity does when it starts. */
    fun installApplication() {
        ReaderNative.appContext = ApplicationProvider.getApplicationContext<Context>()
    }

    /**
     * Drops the process-wide database handles: every Robolectric test gets a fresh data
     * directory, and a handle from an earlier test would point at files that are gone.
     */
    fun resetStores() {
        ReaderSession::class.java.getDeclaredField("database").apply {
            isAccessible = true
            (get(null) as? android.database.sqlite.SQLiteOpenHelper)?.close()
            set(null, null)
        }
        org.ppsspp.ppsspp.reader.dict.DictionaryStore::class.java.getDeclaredField("instance").apply {
            isAccessible = true
            (get(null) as? org.ppsspp.ppsspp.reader.dict.DictionaryStore)?.database?.close()
            set(null, null)
        }
    }

    fun idle() = shadowOf(Looper.getMainLooper()).idle()

    /** Runs the main looper until [condition] holds; background threads get time to finish. */
    fun await(what: String, timeoutMs: Long = 8000, condition: () -> Boolean) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            idle()
            if (condition()) return
            Thread.sleep(20)
        }
        throw AssertionError("timed out waiting for $what")
    }

    private val font: Typeface? by lazy {
        System.getenv("READER_PREVIEW_FONT")?.let(::File)?.takeIf { it.exists() }
            ?.let { Typeface.createFromFile(it) }
    }

    /** The font named by READER_PREVIEW_FONT, if any. */
    fun previewFont(): Typeface? = font

    /** Uses the preview font (when one is configured) so Japanese text is readable in PNGs. */
    fun applyFont(view: View) {
        val typeface = font ?: return
        if (view is TextView) view.typeface = typeface
        if (view is ViewGroup) for (i in 0 until view.childCount) applyFont(view.getChildAt(i))
    }

    /** Lays [root] out at the given size and, when READER_PREVIEW_DIR is set, saves a PNG. */
    fun snapshot(root: View, width: Int, height: Int, name: String) {
        applyFont(root)
        repeat(2) {
            root.measure(
                View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY)
            )
            root.layout(0, 0, width, height)
            idle()
        }
        val dir = System.getenv("READER_PREVIEW_DIR") ?: return
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        root.draw(Canvas(bitmap))
        File(dir).mkdirs()
        File(dir, "$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    fun allText(view: View): String = buildString {
        if (view is TextView) append(view.text).append('\n')
        if (view is ViewGroup) for (i in 0 until view.childCount) append(allText(view.getChildAt(i)))
    }

    fun <T : View> find(view: View, type: Class<T>, test: (T) -> Boolean = { true }): T? {
        if (type.isInstance(view) && test(type.cast(view)!!)) return type.cast(view)
        if (view is ViewGroup) {
            for (i in 0 until view.childCount) find(view.getChildAt(i), type, test)?.let { return it }
        }
        return null
    }
}

/**
 * A stand-in for AnkiConnect speaking its version 6 protocol over real HTTP
 * (request {action, version, params, key}; reply {result, error}). No real Anki is involved.
 */
class AnkiConnectStub {
    val requests = ArrayList<JSONObject>()

    /** Request line and header lines of the latest request. */
    @Volatile
    var head: List<String> = emptyList()
    var requiredKey: String? = null
    var status = 200

    /**
     * Set: the stub stands for VN Library's media add-on in front of AnkiConnect, which
     * refuses a note whose field of this name holds no web address, in the add-on's words.
     */
    var addressField: String? = null

    /** How the reply's length is told: "length" (Content-Length), "chunked" or "close". */
    var framing = "length"
    var decks = listOf("Default", "日本語::Mining")
    var models = mapOf("Basic" to listOf("Front", "Back"), "Lapis" to listOf("Expression", "Sentence", "Picture"))

    /** The notes found for a search, by the search string as the client sent it. */
    @Volatile
    var find: (query: String) -> List<Long> = { emptyList() }

    /** Requests of one kind received so far. */
    fun received(action: String): List<JSONObject> =
        synchronized(requests) { requests.filter { it.getString("action") == action } }

    private val server = ServerSocket(0, 16, InetAddress.getLoopbackAddress())
    val url: String get() = "http://127.0.0.1:${server.localPort}"
    val port: Int get() = server.localPort

    init {
        Thread {
            while (!server.isClosed) {
                val socket = try {
                    server.accept()
                } catch (_: Exception) {
                    break
                }
                socket.use { runCatching { serve(it) } }
            }
        }.start()
    }

    fun close() = server.close()

    private fun serve(socket: Socket) {
        val input = BufferedInputStream(socket.getInputStream())
        var length = 0
        val lines = ArrayList<String>()
        while (true) {
            val line = StringBuilder()
            while (true) {
                val c = input.read()
                if (c < 0 || c == '\n'.code) break
                if (c != '\r'.code) line.append(c.toChar())
            }
            if (line.isEmpty()) break
            lines.add(line.toString())
            if (line.startsWith("content-length:", ignoreCase = true)) {
                length = line.substring(15).trim().toInt()
            }
        }
        head = lines
        val body = ByteArray(length)
        var read = 0
        while (read < length) read += input.read(body, read, length - read)
        val request = JSONObject(String(body, Charsets.UTF_8))
        synchronized(requests) { requests.add(request) }

        val reply = JSONObject()
        val action = request.getString("action")
        val params = request.optJSONObject("params")
        if (requiredKey != null && request.optString("key") != requiredKey) {
            reply.put("result", JSONObject.NULL).put("error", "valid api key must be provided")
        } else if (action == "addNote" &&
            params!!.getJSONObject("note").getJSONObject("fields").optString("Expression") == "重複"
        ) {
            reply.put("result", JSONObject.NULL)
                .put("error", "cannot create note because it is a duplicate")
        } else if (action == "addNote" && addressField != null &&
            !Regex("https?://[^\\s<>\"']+").containsMatchIn(
                params!!.getJSONObject("note").getJSONObject("fields").optString(addressField!!)
            )
        ) {
            val held = params.getJSONObject("note").getJSONObject("fields").optString(addressField!!)
            reply.put("result", JSONObject.NULL).put(
                "error",
                if (held.isBlank()) {
                    "Map $addressField to {url} in Yomitan so game media can be matched safely"
                } else {
                    "The $addressField field must contain Yomitan’s {url}, not the page title"
                }
            )
        } else {
            val result: Any = when (action) {
                "version" -> 6
                "deckNames" -> JSONArray(decks)
                "modelNames" -> JSONArray(models.keys.toList())
                "modelFieldNames" -> JSONArray(models[params!!.getString("modelName")].orEmpty())
                "storeMediaFile" -> params!!.getString("filename")
                "addNote" -> 1496198395707L
                "findNotes" -> JSONArray(find(params!!.getString("query")))
                "guiBrowse" -> JSONArray()
                else -> JSONObject.NULL
            }
            reply.put("result", result).put("error", JSONObject.NULL)
        }
        val bytes = reply.toString().toByteArray(Charsets.UTF_8)
        val reason = if (status == 200) "OK" else "Error"
        socket.getOutputStream().apply {
            val framingHeader = when (framing) {
                "chunked" -> "Transfer-Encoding: chunked\r\n"
                "close" -> ""
                else -> "Content-Length: ${bytes.size}\r\n"
            }
            write(
                (
                    "HTTP/1.1 $status $reason\r\nContent-Type: application/json\r\n" +
                        framingHeader + "Connection: close\r\n\r\n"
                    ).toByteArray(Charsets.US_ASCII)
            )
            if (framing == "chunked") {
                // In pieces that cut through multi-byte characters.
                var at = 0
                while (at < bytes.size) {
                    val size = minOf(7, bytes.size - at)
                    write("${size.toString(16)}\r\n".toByteArray(Charsets.US_ASCII))
                    write(bytes, at, size)
                    write("\r\n".toByteArray(Charsets.US_ASCII))
                    at += size
                }
                write("0\r\n\r\n".toByteArray(Charsets.US_ASCII))
            } else {
                write(bytes)
            }
            flush()
        }
    }
}
