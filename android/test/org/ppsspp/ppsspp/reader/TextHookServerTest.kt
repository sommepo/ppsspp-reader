// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.io.BufferedInputStream
import java.io.DataInputStream
import java.net.HttpURLConnection
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.URL
import java.security.MessageDigest
import java.util.Base64
import org.ppsspp.ppsspp.reader.hook.TextHookServer
import org.ppsspp.ppsspp.reader.model.RubySpan
import org.ppsspp.ppsspp.reader.model.TextLine
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TextHookServerTest {
    private var server: TextHookServer? = null

    @After
    fun tearDown() {
        server?.stop()
    }

    private fun line(text: String, speaker: String = "タロウ") = TextLine(
        sequence = 1, frame = 2, audioPosition = 3, channel = "dialogue", speaker = speaker,
        text = text, ruby = listOf(RubySpan(0, 1, "きみ")), sourceId = "abc", meta = emptyMap(),
        timestamp = 1_700_000_000_000
    )

    /**
     * A WebSocket client written against RFC 6455 for this test (java.net.http is not on the
     * Android test classpath): opening handshake, unmasked server frames, masked client frames.
     */
    private class Client(port: Int) {
        private val socket = Socket("127.0.0.1", port).apply { soTimeout = 5000 }
        private val input = DataInputStream(BufferedInputStream(socket.getInputStream()))
        private val output = socket.getOutputStream()

        init {
            val key = Base64.getEncoder().encodeToString(ByteArray(16) { (it * 7 + 1).toByte() })
            output.write(
                (
                    "GET /hook HTTP/1.1\r\nHost: 127.0.0.1:$port\r\nUpgrade: websocket\r\n" +
                        "Connection: Upgrade\r\nSec-WebSocket-Key: $key\r\n" +
                        "Sec-WebSocket-Version: 13\r\n\r\n"
                    ).toByteArray(Charsets.US_ASCII)
            )
            output.flush()
            val lines = ArrayList<String>()
            val line = StringBuilder()
            while (true) {
                val c = input.read()
                check(c >= 0) { "server closed during the handshake" }
                if (c == '\n'.code) {
                    val text = line.toString().trimEnd('\r')
                    line.setLength(0)
                    if (text.isEmpty()) break
                    lines.add(text)
                } else {
                    line.append(c.toChar())
                }
            }
            check(lines[0].startsWith("HTTP/1.1 101")) { lines[0] }
            val headers = lines.drop(1).associate {
                it.substringBefore(':').trim().lowercase() to it.substringAfter(':').trim()
            }
            check(headers["upgrade"].equals("websocket", ignoreCase = true))
            check(headers["connection"].equals("Upgrade", ignoreCase = true))
            // The server must prove it understood the key (RFC 6455, 4.2.2).
            val expected = Base64.getEncoder().encodeToString(
                MessageDigest.getInstance("SHA-1")
                    .digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toByteArray())
            )
            check(headers["sec-websocket-accept"] == expected)
        }

        /** Opcode and payload of the next frame; server frames must be final and unmasked. */
        fun frame(): Pair<Int, ByteArray> {
            val b0 = input.readUnsignedByte()
            val b1 = input.readUnsignedByte()
            check(b0 and 0x80 != 0) { "fragmented frame" }
            check(b0 and 0x70 == 0) { "reserved bits set" }
            check(b1 and 0x80 == 0) { "server frames must not be masked" }
            val length = when (val short = b1 and 0x7F) {
                126 -> input.readUnsignedShort().toLong()
                127 -> input.readLong()
                else -> short.toLong()
            }
            val payload = ByteArray(length.toInt())
            input.readFully(payload)
            return (b0 and 0x0F) to payload
        }

        /** The next text message, or null when nothing arrives in time. */
        fun next(timeoutMs: Int = 5000): String? {
            socket.soTimeout = timeoutMs
            return try {
                val (opcode, payload) = frame()
                check(opcode == 1) { "expected a text frame, got opcode $opcode" }
                String(payload, Charsets.UTF_8)
            } catch (_: SocketTimeoutException) {
                null
            }
        }

        /** Sends a masked client frame, as the protocol requires of clients. */
        fun send(opcode: Int, payload: ByteArray) {
            val mask = byteArrayOf(0x12, 0x34, 0x56, 0x78)
            val frame = ByteArray(6 + payload.size)
            frame[0] = (0x80 or opcode).toByte()
            frame[1] = (0x80 or payload.size).toByte() // payloads in this test are short
            mask.copyInto(frame, 2)
            for (i in payload.indices) frame[6 + i] = (payload[i].toInt() xor mask[i and 3].toInt()).toByte()
            output.write(frame)
            output.flush()
        }

        fun close() {
            socket.close()
        }
    }

    private fun start(): TextHookServer {
        val s = TextHookServer(0, lan = false)
        assertTrue(s.start())
        server = s
        return s
    }

    private fun awaitClients(s: TextHookServer, count: Int) {
        val deadline = System.currentTimeMillis() + 5000
        while (s.clientCount != count && System.currentTimeMillis() < deadline) Thread.sleep(10)
        assertEquals(count, s.clientCount)
    }

    @Test
    fun acceptKeyMatchesTheRfcExample() {
        // RFC 6455, section 1.3.
        assertEquals("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", TextHookServer.acceptKey("dGhlIHNhbXBsZSBub25jZQ=="))
    }

    @Test
    fun deliversLinesToEveryClientInOrder() {
        val s = start()
        val a = Client(s.boundPort)
        val b = Client(s.boundPort)
        awaitClients(s, 2)
        s.broadcast(line("君は旅人だ？\n道を見張れ"))
        s.broadcast(line("私の名前は　まだない…"))
        for (client in listOf(a, b)) {
            assertEquals("君は旅人だ？\n道を見張れ", client.next())
            assertEquals("私の名前は　まだない…", client.next())
        }
    }

    @Test
    fun aLateClientGetsNothingFromBefore() {
        val s = start()
        s.broadcast(line("before anyone listened"))
        val client = Client(s.boundPort)
        awaitClients(s, 1)
        s.broadcast(line("after"))
        assertEquals("after", client.next())
        assertNull(client.next(timeoutMs = 300))
    }

    @Test
    fun sendsLongMessagesWithExtendedLength() {
        val s = start()
        val client = Client(s.boundPort)
        awaitClients(s, 1)
        val medium = "あ".repeat(100) // 300 bytes: 16-bit length
        val large = "漢".repeat(30_000) // 90,000 bytes: 64-bit length
        s.broadcast(line(medium))
        s.broadcast(line(large))
        assertEquals(medium, client.next())
        assertEquals(large, client.next())
    }

    @Test
    fun formatsFollowTheSetting() {
        val s = start()
        val l = line("君は旅人だ？")
        assertEquals("君は旅人だ？", s.encode(l))
        s.includeSpeaker = true
        assertEquals("タロウ\n君は旅人だ？", s.encode(l))
        s.format = TextHookServer.FORMAT_SENTENCE
        assertEquals("タロウ\n君は旅人だ？", JSONObject(s.encode(l)).getString("sentence"))
        s.format = TextHookServer.FORMAT_JSON
        s.includeSpeaker = false
        s.gameId = "0004000000088A00"
        val event = JSONObject(s.encode(l))
        assertEquals("azahar-reader.text-event", event.getString("format"))
        assertEquals(1, event.getInt("version"))
        assertEquals("0004000000088A00", event.getString("gameId"))
        assertEquals("abc", event.getString("segmentId"))
        assertEquals("タロウ", event.getString("speaker"))
        assertEquals("君は旅人だ？", event.getString("sentence"))
        assertEquals("2023-11-14T22:13:20Z", event.getString("timestamp"))
        assertFalse(event.getBoolean("speakerIncluded"))
        assertEquals("きみ", event.getJSONArray("ruby").getJSONArray(0).getString(2))
    }

    @Test
    fun aClientThatClosesIsAnsweredAndDropped() {
        val s = start()
        val staying = Client(s.boundPort)
        val leaving = Client(s.boundPort)
        awaitClients(s, 2)
        leaving.send(opcode = 8, payload = byteArrayOf(0x03, 0xE8.toByte())) // close, status 1000
        assertEquals(8, leaving.frame().first)
        awaitClients(s, 1)
        s.broadcast(line("still here"))
        assertEquals("still here", staying.next())
    }

    @Test
    fun aClientThatVanishesIsDroppedOnTheNextSend() {
        val s = start()
        val staying = Client(s.boundPort)
        val vanishing = Client(s.boundPort)
        awaitClients(s, 2)
        vanishing.close()
        awaitClients(s, 1)
        s.broadcast(line("still here"))
        assertEquals("still here", staying.next())
    }

    @Test
    fun pingsAreAnsweredWithTheSamePayload() {
        val s = start()
        val client = Client(s.boundPort)
        awaitClients(s, 1)
        client.send(opcode = 9, payload = "are you there".toByteArray())
        val (opcode, payload) = client.frame()
        assertEquals(10, opcode)
        assertEquals("are you there", String(payload))
    }

    @Test
    fun plainHttpRequestsAreToldToUpgrade() {
        val s = start()
        val connection = URL("http://127.0.0.1:${s.boundPort}/").openConnection() as HttpURLConnection
        assertEquals(426, connection.responseCode)
        connection.disconnect()
    }

    @Test
    fun listensOnLoopbackOnlyByDefault() {
        val s = start()
        assertTrue(s.describeAddress().startsWith("ws://127.0.0.1:"))
    }

    @Test
    fun aBusyPortIsReportedNotThrown() {
        val s = start()
        val second = TextHookServer(s.boundPort, lan = false)
        assertFalse(second.start())
        second.stop()
    }
}
