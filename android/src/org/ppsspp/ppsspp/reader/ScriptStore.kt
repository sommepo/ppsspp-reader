// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the ARMSX2 Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.io.File
import java.net.HttpURLConnection
import java.net.URI
import java.net.URL
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import org.json.JSONArray
import org.json.JSONObject

/**
 * Game scripts (text profiles) to install: the list this repository publishes of them
 * (`assets/reader/profiles/index.json`, made by tools/reader/scripts-index.py), what is
 * installed and how it compares, and installing and removing one in the app's profiles folder.
 * The same rules as the desktop app's `reader/desktop/script_index.*`.
 */
object ScriptStore {
    /** The version of the profile API this app has. A script asks for one with `-- reader: api N`. */
    const val API = 1

    /** Where the list is fetched from. */
    const val DEFAULT_INDEX_URL =
        "https://raw.githubusercontent.com/sommepo/ppsspp-reader/main/assets/reader/profiles/index.json"

    /** What fetches a file: the network on a device, something else in tests. */
    @Volatile
    var fetcher: (String) -> ByteArray = ::download

    @Volatile
    var indexUrl: String = DEFAULT_INDEX_URL

    private const val INDEX_FORMAT = 1
    private const val SCRIPT_MAX = 1L shl 20
    private const val DATA_MAX = 16L shl 20
    private const val RECORD = ".installed.json"
    private val FILE_REST = Regex("[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*")
    private val API_LINE = Regex("^--\\s*reader:\\s*api\\s+(\\d+)\\s*$")

    data class FileEntry(val file: String, val sha256: String, val size: Long)

    data class Entry(
        val id: String,
        val title: String,
        val reads: String,
        val version: String,
        val api: Int,
        val script: FileEntry,
        val data: List<FileEntry>,
    )

    enum class State { NOT_INSTALLED, INSTALLED, UPDATE, OWN, TOO_NEW }

    class ScriptException(message: String) : Exception(message)

    /** The game a file of this name belongs to ("<ID>.lua", "<ID>.<name>"), or null. */
    fun idOfFile(name: String): String? {
        val dot = name.indexOf('.')
        if (dot <= 0 || !FILE_REST.matches(name.substring(dot + 1))) return null
        val id = name.substring(0, dot)
        return id.takeIf { ReaderCatalog.titleIdOf("$it.lua") == it }
    }

    fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    /** The API a script's source says it needs; 1 when it says nothing. */
    fun apiOf(source: String): Int {
        for (raw in source.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            if (!line.startsWith("--")) break
            API_LINE.matchEntire(line)?.let { return it.groupValues[1].toInt() }
        }
        return 1
    }

    /** Reads a list; throws [ScriptException] when it is not one this app understands. */
    fun parseIndex(json: ByteArray): List<Entry> {
        val root = try {
            JSONObject(String(json, Charsets.UTF_8))
        } catch (e: Exception) {
            throw ScriptException("The list of scripts is not JSON")
        }
        val format = root.optInt("format", 0)
        if (format < 1 || format > INDEX_FORMAT) {
            throw ScriptException("The list of scripts is in a format this app does not know ($format): a newer app may read it")
        }
        val scripts = root.optJSONArray("scripts") ?: JSONArray()
        val out = ArrayList<Entry>()
        for (i in 0 until scripts.length()) {
            val o = scripts.optJSONObject(i) ?: continue
            val id = o.optString("id").trim()
            val script = fileOf(o) ?: continue
            // An entry whose file is not its game's script is passed over.
            if (ReaderCatalog.titleIdOf(script.file) != id) continue
            val data = ArrayList<FileEntry>()
            var whole = true
            val list = o.optJSONArray("data") ?: JSONArray()
            for (j in 0 until list.length()) {
                val file = list.optJSONObject(j)?.let(::fileOf)
                if (file == null || idOfFile(file.file) != id || file.file == script.file) {
                    whole = false
                    break
                }
                data.add(file)
            }
            if (!whole) continue
            out.add(Entry(
                id,
                o.optString("title").trim().ifEmpty { id },
                o.optString("reads").trim(),
                o.optString("version").trim(),
                o.optInt("api", 1),
                script,
                data,
            ))
        }
        return out
    }

    private fun fileOf(o: JSONObject): FileEntry? {
        val file = FileEntry(o.optString("file").trim(), o.optString("sha256").trim().lowercase(), o.optLong("size", -1))
        return file.takeIf { idOfFile(it.file) != null && it.sha256.length == 64 && it.size >= 0 }
    }

    /** How [entry] compares with the user's script, the bundled one and what was recorded. */
    fun stateOf(entry: Entry, user: ByteArray?, bundled: ByteArray?, recorded: String): State {
        if (entry.api > API) return State.TOO_NEW
        if (user != null) {
            val sha = sha256(user)
            if (sha == entry.script.sha256) return State.INSTALLED
            return if (recorded.isNotEmpty() && sha == recorded) State.UPDATE else State.OWN
        }
        if (bundled != null) return if (sha256(bundled) == entry.script.sha256) State.INSTALLED else State.UPDATE
        return State.NOT_INSTALLED
    }

    /** The address of a file the list names: beside the list. */
    fun urlOf(file: String, index: String = indexUrl): String = URI(index).resolve(file).toString()

    private fun download(address: String): ByteArray {
        val connection = URL(address).openConnection() as HttpURLConnection
        connection.connectTimeout = 15_000
        connection.readTimeout = 30_000
        try {
            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw ScriptException("Could not fetch $address ($code)")
            return connection.inputStream.use { it.readBytes() }
        } finally {
            connection.disconnect()
        }
    }

    private fun isUtf8(bytes: ByteArray): Boolean = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
        true
    } catch (_: CharacterCodingException) {
        false
    }

    /** The user's profiles folder: what is in it, and what came from the list (`.installed.json`). */
    class Folder(val dir: File) {
        fun script(id: String): ByteArray? = File(dir, "$id.lua").takeIf { it.isFile }?.readBytes()

        fun recorded(id: String): String = record().optJSONObject(id)?.optString("sha256").orEmpty()

        private fun record(): JSONObject = try {
            JSONObject(File(dir, RECORD).readText())
        } catch (_: Exception) {
            JSONObject()
        }

        private fun write(name: String, bytes: ByteArray) {
            if (!dir.isDirectory && !dir.mkdirs()) throw ScriptException("Cannot make the folder $dir")
            val tmp = File(dir, "$name.part")
            tmp.writeBytes(bytes)
            if (!tmp.renameTo(File(dir, name))) {
                tmp.delete()
                throw ScriptException("Cannot write $name")
            }
        }

        /**
         * Installs the list's script of [entry] and its data files, [files] holding the bytes of
         * each by name, after checking every one against the list and the script against this
         * app's API. Throws [ScriptException] when a check fails.
         */
        fun install(entry: Entry, files: Map<String, ByteArray>) {
            fun check(wanted: FileEntry): ByteArray {
                val bytes = files[wanted.file] ?: throw ScriptException("${wanted.file} was not fetched")
                if (idOfFile(wanted.file) != entry.id) throw ScriptException("${wanted.file} is not a file of ${entry.id}")
                if (bytes.size.toLong() != wanted.size || sha256(bytes) != wanted.sha256) {
                    throw ScriptException("${wanted.file} is not the file the list names (it may have changed since the list was read: try again)")
                }
                return bytes
            }
            if (entry.api > API) throw ScriptException("This script needs a newer app")
            val source = check(entry.script)
            if (!isUtf8(source) || apiOf(String(source, Charsets.UTF_8)) > API) {
                throw ScriptException("${entry.script.file} is not a script this app can run")
            }
            val data = entry.data.map { it.file to check(it) }
            // The data first: a script whose data is missing says so, data without its script is never asked for.
            for ((name, bytes) in data) write(name, bytes)
            write(entry.script.file, source)
            val root = record()
            root.put(entry.id, JSONObject().put("sha256", entry.script.sha256).put("data", JSONArray(data.map { it.first })))
            write(RECORD, root.toString(2).toByteArray())
        }

        /**
         * Installs files the user chose, by name: `<ID>.lua` a script, `<ID>.<name>` a data
         * file. Returns the IDs of the scripts installed and what was not taken, and why.
         */
        fun installFiles(files: List<Pair<String, ByteArray>>): Pair<List<String>, List<String>> {
            val installed = ArrayList<String>()
            val problems = ArrayList<String>()
            val root = record()
            for ((name, bytes) in files) {
                val id = idOfFile(name)
                if (id == null) {
                    problems.add("$name: not a game's script or data file (a script is named after its game, as ULJS00219.lua)")
                    continue
                }
                val script = name.endsWith(".lua")
                if (bytes.size > (if (script) SCRIPT_MAX else DATA_MAX)) {
                    problems.add("$name: too large")
                    continue
                }
                if (script && (ReaderCatalog.titleIdOf(name) == null || !isUtf8(bytes))) {
                    problems.add("$name: not a script (a script is UTF-8 text)")
                    continue
                }
                if (script && apiOf(String(bytes, Charsets.UTF_8)) > API) {
                    problems.add("$name: needs a newer app")
                    continue
                }
                try {
                    write(name, bytes)
                } catch (e: ScriptException) {
                    problems.add(e.message.orEmpty())
                    continue
                }
                if (script) {
                    installed.add(id)
                    root.remove(id) // the user's own now
                }
            }
            runCatching { write(RECORD, root.toString(2).toByteArray()) }
            return installed to problems
        }

        /** Removes the game's script and the data files that came with it from the list. */
        fun remove(id: String): Boolean {
            val root = record()
            val data = root.optJSONObject(id)?.optJSONArray("data") ?: JSONArray()
            for (i in 0 until data.length()) {
                val name = data.optString(i)
                if (idOfFile(name) == id) File(dir, name).delete()
            }
            val had = File(dir, "$id.lua").delete()
            if (root.has(id)) {
                root.remove(id)
                runCatching { write(RECORD, root.toString(2).toByteArray()) }
            }
            return had
        }
    }
}
