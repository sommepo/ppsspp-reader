// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Azahar Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import android.content.Context
import java.io.BufferedInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors
import org.json.JSONObject

/**
 * Sends this install's reading statistics to Eden Reader's save sync service, where the
 * reader apps' records are kept side by side (one per install, whole each time) for whoever
 * adds them up. Nothing is sent until an address and the service's code are entered (Reader
 * settings, Statistics options) or save sync, which the emulator's side does, is set up.
 *
 * The request is `PUT /v1/stats/<install>` with `Authorization: Bearer <code>`, over a plain
 * socket: the service is reached inside the home network or over Tailscale.
 */
object StatsSync {
    const val DEFAULT_PORT = 8642
    private const val CONNECT_TIMEOUT_MS = 4_000
    private const val READ_TIMEOUT_MS = 10_000

    data class Address(val host: String, val port: Int)

    private val worker = Executors.newSingleThreadExecutor { Thread(it, "ReaderStatsSync") }

    /** What the last attempt came to, for the settings page: "" before any. */
    @Volatile
    var lastResult: String = ""
        private set

    /**
     * The addresses as typed: "host", "host:port" or "http://host:port", several separated by
     * commas, semicolons or spaces. What is no address is left out.
     */
    fun parseAddresses(text: String): List<Address> = text.split(',', ';', ' ', '\n', '\t')
        .map { it.trim().removePrefix("http://").trimEnd('/') }
        .filter { it.isNotEmpty() && !it.startsWith("https://") }
        .mapNotNull { entry ->
            val colon = entry.lastIndexOf(':')
            val host = if (colon > 0) entry.substring(0, colon) else entry
            val port = if (colon > 0) entry.substring(colon + 1).toIntOrNull() ?: return@mapNotNull null else DEFAULT_PORT
            if (host.isEmpty() || host.any { it == '/' || it.isWhitespace() } || port !in 1..65535) null
            else Address(host, port)
        }

    /** The record: every reading day of every game, and which install and device it is from. */
    fun record(context: Context): JSONObject {
        val settings = ReaderSettings(context)
        return ReaderSession.database(context).statsRecord()
            .put("app", "ppsspp-reader")
            .put("platform", "android")
            .put("device", settings.statsDevice)
            .put("sent", System.currentTimeMillis())
    }

    /**
     * Save sync's address and code where the app has them (set by the glue to the emulator):
     * used while none are entered under Statistics options, so that they are typed once.
     */
    @Volatile
    var saveSyncService: (() -> Pair<String, String>?)? = null

    /** The address and the code to send to: the ones entered here, else save sync's. */
    internal fun service(settings: ReaderSettings): Pair<String, String>? {
        if (parseAddresses(settings.statsServer).isNotEmpty() && settings.statsCode.isNotEmpty()) {
            return settings.statsServer to settings.statsCode
        }
        val other = try { saveSyncService?.invoke() } catch (e: Throwable) { null } ?: return null
        return if (parseAddresses(other.first).isNotEmpty() && other.second.isNotEmpty()) other else null
    }

    /** True when there is an address and a code to send to. */
    fun isSetUp(settings: ReaderSettings) = service(settings) != null

    /**
     * Sends the record in the background: after a game stopped and when the app starts.
     * Nothing is said when it does not arrive; the next record is whole again. [then] hears
     * the outcome (for the settings page's "Send now").
     */
    fun send(context: Context, then: ((Boolean, String) -> Unit)? = null) {
        val app = context.applicationContext
        worker.execute {
            val settings = ReaderSettings(app)
            val service = service(settings)
            if (service == null) {
                then?.invoke(false, "No address or no code")
                return@execute
            }
            // What the game that just stopped counted last is written by then.
            ReaderSession.awaitStopped(5_000)
            val body = record(app).toString().toByteArray(Charsets.UTF_8)
            var said = "No address answered"
            var sent = false
            for (address in parseAddresses(service.first)) {
                try {
                    val status = put(address, "/v1/stats/${settings.statsInstall}", service.second, body)
                    said = when (status) {
                        200 -> "Sent to ${address.host}"
                        401 -> "The code is not the service's"
                        else -> "The service answered $status"
                    }
                    sent = status == 200
                    break
                } catch (e: Exception) {
                    said = "No answer from ${address.host}:${address.port}"
                }
            }
            lastResult = said
            then?.invoke(sent, said)
        }
    }

    /** One HTTP/1.1 PUT over a plain socket; the status of the answer. */
    internal fun put(address: Address, path: String, code: String, body: ByteArray): Int {
        Socket().use { socket ->
            socket.connect(InetSocketAddress(address.host, address.port), CONNECT_TIMEOUT_MS)
            socket.soTimeout = READ_TIMEOUT_MS
            val out = socket.getOutputStream()
            val host = if (address.host.contains(':')) "[${address.host}]" else address.host
            out.write(
                (
                    "PUT $path HTTP/1.1\r\nHost: $host:${address.port}\r\n" +
                        "Authorization: Bearer ${code.filter { it.code in 0x21..0x7e }}\r\n" +
                        "Content-Type: application/json\r\nContent-Length: ${body.size}\r\n" +
                        "Connection: close\r\n\r\n"
                    ).toByteArray(Charsets.US_ASCII)
            )
            out.write(body)
            out.flush()
            val input = BufferedInputStream(socket.getInputStream())
            val line = StringBuilder()
            while (true) {
                val c = input.read()
                if (c < 0 || c == '\n'.code) break
                if (c != '\r'.code) line.append(c.toChar())
            }
            return line.toString().split(' ').getOrNull(1)?.toIntOrNull()
                ?: throw java.io.IOException("not HTTP")
        }
    }
}
