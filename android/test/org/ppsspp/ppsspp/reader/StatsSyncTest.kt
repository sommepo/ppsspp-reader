// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Azahar Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import androidx.preference.PreferenceManager
import androidx.test.core.app.ApplicationProvider
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.ppsspp.ppsspp.reader.stats.ReadingCounters
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

/** The statistics record, where it is sent, and the button that pauses the statistics. */
@RunWith(RobolectricTestRunner::class)
class StatsSyncTest {
    private val context: Context = ApplicationProvider.getApplicationContext()
    private val prefs get() = PreferenceManager.getDefaultSharedPreferences(context)
    private val game = "0004000000088A00"

    @Before
    fun setUp() {
        prefs.edit().clear().commit()
    }

    @After
    fun tearDown() {
        prefs.edit().clear().commit()
    }

    @Test
    fun addressesAsTheyAreTyped() {
        val a = StatsSync::parseAddresses
        assertEquals(listOf(StatsSync.Address("192.168.1.50", 8642)), a("192.168.1.50"))
        assertEquals(
            listOf(StatsSync.Address("home.example.ts.net", 8642), StatsSync.Address("192.168.1.50", 9000)),
            a(" http://home.example.ts.net:8642/ , 192.168.1.50:9000 ")
        )
        assertEquals(emptyList<StatsSync.Address>(), a(""))
        assertEquals(emptyList<StatsSync.Address>(), a("host:notaport, https://secure.example, host:0"))
    }

    @Test
    fun theRecordHasEveryReadingDayOfEveryGameAndSaysWhereItIsFrom() {
        val db = ReaderSession.database(context)
        db.clearActivity()
        val s1 = db.createSession(game, 1000)
        val s2 = db.createSession(game, 5000)
        db.addActivity(s1, game, "2026-10-01", ReadingCounters(60_000, 100, 80, 20, 5, 7), 1500)
        db.addActivity(s2, game, "2026-10-01", ReadingCounters(30_000, 50, 50, 0, 2, 0), 5800)
        db.addActivity(s2, game, "2026-10-02", ReadingCounters(10_000, 10, 10, 0, 1, 0), 5900)
        db.setGameTitle(game, "真・女神転生IV")
        prefs.edit().putString(ReaderSettings.KEY_STATS_DEVICE, "Thor").commit()

        val record = StatsSync.record(context)
        assertEquals("ppsspp-reader", record.getString("app"))
        assertEquals("android", record.getString("platform"))
        assertEquals("Thor", record.getString("device"))
        assertEquals("真・女神転生IV", record.getJSONObject("games").getString(game))
        val days = record.getJSONArray("days")
        assertEquals(2, days.length())
        val first = days.getJSONObject(0)
        assertEquals("2026-10-01", first.getString("day"))
        assertEquals(game, first.getString("game"))
        assertEquals(150L, first.getLong("chars"))
        assertEquals(90_000L, first.getLong("active_ms"))
        assertEquals(7L, first.getLong("lines"))
        assertEquals(2L, first.getLong("sessions"))
        val install = ReaderSettings(context).statsInstall
        assertTrue(install, Regex("[0-9a-f]{16}").matches(install))
        assertEquals("made up once and kept", install, ReaderSettings(context).statsInstall)
        db.clearActivity()
    }

    @Test
    fun theRecordGoesToTheServiceWithItsCodeAndNothingIsSentWithoutASetup() {
        var path = ""
        var authorisation = ""
        var body = ""
        var answer = 200
        // A stand-in for the service: one request per connection, answered with [answer].
        val server = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        val serving = Thread {
            while (!server.isClosed) {
                try {
                    server.accept().use { socket ->
                        val input = socket.getInputStream()
                        val head = StringBuilder()
                        while (!head.endsWith("\r\n\r\n")) {
                            val c = input.read()
                            if (c < 0) break
                            head.append(c.toChar())
                        }
                        val lines = head.toString().split("\r\n")
                        val length = lines.firstOrNull { it.startsWith("Content-Length:", true) }
                            ?.substringAfter(':')?.trim()?.toInt() ?: 0
                        val bytes = ByteArray(length)
                        var read = 0
                        while (read < length) {
                            val n = input.read(bytes, read, length - read)
                            if (n < 0) break
                            read += n
                        }
                        path = lines[0].substringBeforeLast(' ')
                        authorisation = lines.firstOrNull { it.startsWith("Authorization:", true) }
                            ?.substringAfter(':')?.trim().orEmpty()
                        body = bytes.toString(Charsets.UTF_8)
                        socket.getOutputStream().write(
                            "HTTP/1.1 $answer X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".toByteArray()
                        )
                    }
                } catch (_: Exception) {
                    // Closed at the end of the test.
                }
            }
        }.apply { isDaemon = true; start() }
        try {
            fun send(): Pair<Boolean, String> {
                val done = CountDownLatch(1)
                var result = false to ""
                StatsSync.send(context) { sent, said ->
                    result = sent to said
                    done.countDown()
                }
                assertTrue(done.await(20, TimeUnit.SECONDS))
                return result
            }
            // Nothing entered: nothing is sent.
            assertFalse(StatsSync.isSetUp(ReaderSettings(context)))
            assertFalse(send().first)
            assertEquals("", path)

            // Nothing entered here, but save sync is set up: its address and code are used.
            StatsSync.saveSyncService = { "127.0.0.1:${server.localPort}" to "SAVE-SYNC-CODE" }
            try {
                assertTrue(StatsSync.isSetUp(ReaderSettings(context)))
                assertTrue(send().first)
                assertEquals("Bearer SAVE-SYNC-CODE", authorisation)
                // One that fails, or has no address, is as good as none.
                StatsSync.saveSyncService = { throw UnsatisfiedLinkError("no native side here") }
                assertFalse(StatsSync.isSetUp(ReaderSettings(context)))
                StatsSync.saveSyncService = { "" to "SAVE-SYNC-CODE" }
                assertFalse(StatsSync.isSetUp(ReaderSettings(context)))
            } finally {
                StatsSync.saveSyncService = null
            }
            path = ""

            // An address that does not answer first, then the one that does.
            prefs.edit()
                .putString(ReaderSettings.KEY_STATS_SERVER, "127.0.0.1:1, 127.0.0.1:${server.localPort}")
                .putString(ReaderSettings.KEY_STATS_CODE, "ABCD-EFGH-JKLM")
                .commit()
            assertTrue(StatsSync.isSetUp(ReaderSettings(context)))
            val (sent, said) = send()
            assertTrue(said, sent)
            assertEquals("PUT /v1/stats/" + ReaderSettings(context).statsInstall, path)
            assertEquals("Bearer ABCD-EFGH-JKLM", authorisation)
            assertEquals("ppsspp-reader", JSONObject(body).getString("app"))
            assertTrue(JSONObject(body).has("days"))

            // The wrong code is said as that.
            answer = 401
            val refused = send()
            assertFalse(refused.first)
            assertEquals("The code is not the service's", refused.second)
        } finally {
            server.close()
            serving.join(2000)
        }
    }
}
