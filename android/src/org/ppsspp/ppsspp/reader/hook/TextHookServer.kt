// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.hook

import java.io.BufferedInputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.time.Instant
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.reader.model.TextLine
import org.json.JSONObject

/**
 * A small WebSocket server that sends every newly presented line to connected clients, the way
 * texthooker pages and sentence-mining tools expect. Nothing is replayed to a client that
 * connects later, and nothing a client sends is acted on.
 *
 * It listens on the loopback interface unless [lan] is set. There is no authentication: anything
 * that can reach the port can read the text, which is why other devices are opt-in.
 */
class TextHookServer(val port: Int, val lan: Boolean) {
    @Volatile
    var format: String = FORMAT_PLAIN

    @Volatile
    var includeSpeaker: Boolean = false

    @Volatile
    var gameId: String = ""

    private var serverSocket: ServerSocket? = null
    private val clients = CopyOnWriteArrayList<Client>()
    private var sender: ExecutorService? = null

    @Volatile
    private var running = false

    /** The port actually bound; differs from [port] only when that was 0. */
    val boundPort: Int get() = serverSocket?.localPort ?: -1

    val clientCount: Int get() = clients.size

    /** Starts listening. Returns false when the port cannot be bound. */
    fun start(): Boolean {
        if (running) return true
        return try {
            val socket = ServerSocket()
            socket.reuseAddress = true
            val address = if (lan) InetAddress.getByName("0.0.0.0") else InetAddress.getLoopbackAddress()
            socket.bind(InetSocketAddress(address, port))
            serverSocket = socket
            running = true
            sender = Executors.newSingleThreadExecutor { r -> Thread(r, "ReaderHookSend") }
            Thread({ acceptLoop(socket) }, "ReaderHookAccept").start()
            true
        } catch (_: IOException) {
            running = false
            false
        }
    }

    fun stop() {
        running = false
        try {
            serverSocket?.close()
        } catch (_: IOException) {
        }
        serverSocket = null
        for (client in clients) client.close()
        clients.clear()
        sender?.shutdownNow()
        sender = null
    }

    /** Queues [line] for every connected client. Safe to call from the main thread. */
    fun broadcast(line: TextLine) {
        if (!running || clients.isEmpty()) return
        val payload = encode(line).toByteArray(Charsets.UTF_8)
        val frame = textFrame(payload)
        sender?.execute {
            for (client in clients) {
                if (!client.send(frame)) {
                    client.close()
                    clients.remove(client)
                }
            }
        }
    }

    /** The message sent for [line] in the current [format]. */
    fun encode(line: TextLine): String {
        val sentence = if (includeSpeaker && line.speaker.isNotEmpty()) {
            "${line.speaker}\n${line.text}"
        } else {
            line.text
        }
        return when (format) {
            FORMAT_SENTENCE -> JSONObject().put("sentence", sentence).toString()

            FORMAT_JSON -> JSONObject().apply {
                put("format", "azahar-reader.text-event")
                put("version", 1)
                put("gameId", gameId)
                put("segmentId", line.sourceId)
                put("channel", line.channel)
                put("speaker", line.speaker)
                put("sentence", sentence)
                put("ruby", TextLine.rubyToJson(line.ruby))
                put("timestamp", Instant.ofEpochMilli(line.timestamp).toString())
                put("speakerIncluded", includeSpeaker && line.speaker.isNotEmpty())
            }.toString()

            else -> sentence
        }
    }

    /** A ws:// address for the settings screen. */
    fun describeAddress(): String {
        val host = if (lan) lanAddress() ?: "127.0.0.1" else "127.0.0.1"
        return "ws://$host:$boundPort"
    }

    private fun lanAddress(): String? = try {
        NetworkInterface.getNetworkInterfaces().asSequence()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.asSequence() }
            .firstOrNull { it is java.net.Inet4Address && !it.isLoopbackAddress }
            ?.hostAddress
    } catch (_: Exception) {
        null
    }

    private fun acceptLoop(socket: ServerSocket) {
        while (running) {
            val connection = try {
                socket.accept()
            } catch (_: IOException) {
                break
            }
            Thread({ serve(connection) }, "ReaderHookClient").start()
        }
    }

    private fun serve(socket: Socket) {
        try {
            socket.tcpNoDelay = true
            socket.soTimeout = HANDSHAKE_TIMEOUT_MS
            val input = BufferedInputStream(socket.getInputStream())
            val output = socket.getOutputStream()
            val headers = readHeaders(input) ?: run {
                socket.close()
                return
            }
            val key = headers["sec-websocket-key"]
            if (key == null || headers["upgrade"]?.lowercase() != "websocket") {
                val body = "PPSSPP Reader text server. Connect with a WebSocket client.\n"
                output.write(
                    (
                        "HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain; charset=utf-8\r\n" +
                            "Content-Length: ${body.length}\r\nConnection: close\r\n\r\n$body"
                        ).toByteArray(Charsets.US_ASCII)
                )
                output.flush()
                socket.close()
                return
            }
            output.write(
                (
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
                        "Connection: Upgrade\r\nSec-WebSocket-Accept: ${acceptKey(key)}\r\n\r\n"
                    ).toByteArray(Charsets.US_ASCII)
            )
            output.flush()
            socket.soTimeout = 0
            val client = Client(socket, output)
            clients.add(client)
            try {
                readFrames(input, client)
            } finally {
                clients.remove(client)
                client.close()
            }
        } catch (_: IOException) {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }

    /** Reads the request line and headers; header names are lower-cased. */
    private fun readHeaders(input: InputStream): Map<String, String>? {
        val headers = HashMap<String, String>()
        val line = StringBuilder()
        var total = 0
        var first = true
        while (true) {
            val b = input.read()
            if (b < 0 || ++total > MAX_HEADER_BYTES) return null
            if (b == '\n'.code) {
                val text = line.toString().trimEnd('\r')
                line.setLength(0)
                if (text.isEmpty()) return headers
                if (first) {
                    first = false
                    if (!text.startsWith("GET ")) return null
                } else {
                    val colon = text.indexOf(':')
                    if (colon > 0) {
                        headers[text.substring(0, colon).trim().lowercase()] =
                            text.substring(colon + 1).trim()
                    }
                }
            } else {
                line.append(b.toChar())
            }
        }
    }

    /** Consumes client frames: answers pings, stops at a close frame or end of stream. */
    private fun readFrames(input: InputStream, client: Client) {
        while (running) {
            val b0 = input.read()
            val b1 = input.read()
            if (b0 < 0 || b1 < 0) return
            val opcode = b0 and 0x0F
            val masked = (b1 and 0x80) != 0
            var length = (b1 and 0x7F).toLong()
            if (length == 126L) {
                length = (readByte(input).toLong() shl 8) or readByte(input).toLong()
            } else if (length == 127L) {
                length = 0
                repeat(8) { length = (length shl 8) or readByte(input).toLong() }
            }
            if (length < 0 || length > MAX_CLIENT_FRAME) return
            val mask = ByteArray(4)
            if (masked) readFully(input, mask)
            val payload = ByteArray(length.toInt())
            readFully(input, payload)
            if (masked) {
                for (i in payload.indices) {
                    payload[i] = (payload[i].toInt() xor mask[i and 3].toInt()).toByte()
                }
            }
            when (opcode) {
                OPCODE_CLOSE -> {
                    client.send(frame(OPCODE_CLOSE, ByteArray(0)))
                    return
                }

                OPCODE_PING -> client.send(frame(OPCODE_PONG, payload))
            }
        }
    }

    private fun readByte(input: InputStream): Int {
        val b = input.read()
        if (b < 0) throw IOException("end of stream")
        return b
    }

    private fun readFully(input: InputStream, buffer: ByteArray) {
        var read = 0
        while (read < buffer.size) {
            val n = input.read(buffer, read, buffer.size - read)
            if (n < 0) throw IOException("end of stream")
            read += n
        }
    }

    private class Client(private val socket: Socket, private val output: OutputStream) {
        @Synchronized
        fun send(frame: ByteArray): Boolean = try {
            output.write(frame)
            output.flush()
            true
        } catch (_: IOException) {
            false
        }

        fun close() {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }

    companion object {
        const val FORMAT_PLAIN = "plain"
        const val FORMAT_SENTENCE = "sentence"
        const val FORMAT_JSON = "json"

        private const val HANDSHAKE_TIMEOUT_MS = 10_000
        private const val MAX_HEADER_BYTES = 16 * 1024
        private const val MAX_CLIENT_FRAME = 64 * 1024L
        private const val OPCODE_TEXT = 0x1
        private const val OPCODE_CLOSE = 0x8
        private const val OPCODE_PING = 0x9
        private const val OPCODE_PONG = 0xA
        private const val WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

        /** The Sec-WebSocket-Accept value for a client's key (RFC 6455, section 4.2.2). */
        fun acceptKey(key: String): String {
            val digest = MessageDigest.getInstance("SHA-1")
                .digest((key + WEBSOCKET_GUID).toByteArray(Charsets.US_ASCII))
            return Base64.getEncoder().encodeToString(digest)
        }

        fun textFrame(payload: ByteArray): ByteArray = frame(OPCODE_TEXT, payload)

        /** A final, unmasked server frame. */
        fun frame(opcode: Int, payload: ByteArray): ByteArray {
            val header = when {
                payload.size < 126 -> byteArrayOf((0x80 or opcode).toByte(), payload.size.toByte())

                payload.size <= 0xFFFF -> byteArrayOf(
                    (0x80 or opcode).toByte(),
                    126,
                    (payload.size shr 8).toByte(),
                    payload.size.toByte()
                )

                else -> ByteArray(10).also {
                    it[0] = (0x80 or opcode).toByte()
                    it[1] = 127
                    val size = payload.size.toLong()
                    for (i in 0 until 8) it[2 + i] = (size shr (8 * (7 - i))).toByte()
                }
            }
            return header + payload
        }
    }
}
