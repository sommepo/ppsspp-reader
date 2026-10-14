// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.util.Base64
import org.ppsspp.ppsspp.reader.anki.AnkiConnectClient
import org.ppsspp.ppsspp.reader.anki.AnkiException
import org.ppsspp.ppsspp.reader.anki.AnkiSearch
import org.ppsspp.ppsspp.reader.anki.Duplicates
import org.ppsspp.ppsspp.reader.anki.SourceAddress
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * The AnkiConnect client against [AnkiConnectStub], a stand-in server that speaks the
 * version 6 protocol. No real Anki is involved.
 */
class AnkiConnectClientTest {
    @get:Rule
    val folder = TemporaryFolder()

    private lateinit var stub: AnkiConnectStub
    private val requests get() = stub.requests

    @Before
    fun setUp() {
        stub = AnkiConnectStub()
    }

    @After
    fun tearDown() {
        stub.close()
    }

    private fun client(key: String = "") = AnkiConnectClient(stub.url, key)

    @Test
    fun listsDecksNoteTypesAndFields() {
        val anki = client()
        assertEquals(6, anki.version())
        assertEquals(listOf("Default", "日本語::Mining"), anki.deckNames())
        assertEquals(listOf("Basic", "Lapis"), anki.modelNames())
        assertEquals(listOf("Expression", "Sentence", "Picture"), anki.fieldNames("Lapis"))
        val fields = requests.last()
        assertEquals("modelFieldNames", fields.getString("action"))
        assertEquals(6, fields.getInt("version"))
        assertEquals("Lapis", fields.getJSONObject("params").getString("modelName"))
        assertFalse("no key is sent when none is set", fields.has("key"))
    }

    @Test
    fun speaksPlainHttpItself() {
        // Android's own HTTP client refuses plain HTTP, which is all AnkiConnect speaks, so the
        // request is written to a socket. What goes over the wire has to be right.
        val anki = AnkiConnectClient(stub.url + "/", "")
        assertEquals(6, anki.version())
        val head = stub.head
        assertEquals("POST / HTTP/1.1", head[0])
        assertTrue(head.toString(), head.contains("Host: 127.0.0.1:${stub.port}"))
        assertTrue(head.toString(), head.contains("Content-Type: application/json"))
        assertTrue(head.toString(), head.any { it.startsWith("Content-Length: ") })

        // However the server tells where its reply ends.
        stub.framing = "chunked"
        assertEquals(listOf("Default", "日本語::Mining"), anki.deckNames())
        stub.framing = "close"
        assertEquals(listOf("Default", "日本語::Mining"), anki.deckNames())

        // A path in the address is kept; an address without a path is a request for "/".
        AnkiConnectClient(stub.url + "/anki?x=1", "").version()
        assertEquals("POST /anki?x=1 HTTP/1.1", stub.head[0])
        AnkiConnectClient(stub.url, "").version()
        assertEquals("POST / HTTP/1.1", stub.head[0])
    }

    @Test
    fun anAddressWithoutHttpIsTakenAsHttpAndNonsenseIsRefusedWithAReason() {
        // What people type: host and port only.
        assertEquals(6, AnkiConnectClient(" 127.0.0.1:${stub.port} ", "").version())
        for (address in listOf("ftp://computer:8765", "", "   ")) {
            val error = assertThrows(AnkiException::class.java) { AnkiConnectClient(address, "").version() }
            assertTrue(error.message, error.message!!.contains("http://"))
        }
    }

    @Test
    fun storesMediaInlineAndAddsTheNote() {
        val anki = client()
        val file = folder.newFile("shot.jpg").apply { writeBytes(byteArrayOf(1, 2, 3, -1)) }
        assertEquals("azahar_1.jpg", anki.storeMedia(file, "azahar_1.jpg"))
        val media = requests.last().getJSONObject("params")
        assertEquals("azahar_1.jpg", media.getString("filename"))
        assertEquals(listOf<Byte>(1, 2, 3, -1), Base64.getDecoder().decode(media.getString("data")).toList())

        val id = anki.addNote(
            deck = "日本語::Mining",
            model = "Lapis",
            fields = mapOf("Expression" to "見張る", "Sentence" to "道を<b>見張れ</b>"),
            tags = listOf("azahar-reader", "smt4"),
            duplicates = Duplicates.REFUSE_IN_DECK
        )
        assertEquals(1496198395707L, id)
        val note = requests.last().getJSONObject("params").getJSONObject("note")
        assertEquals("日本語::Mining", note.getString("deckName"))
        assertEquals("Lapis", note.getString("modelName"))
        assertEquals("道を<b>見張れ</b>", note.getJSONObject("fields").getString("Sentence"))
        assertEquals(listOf("azahar-reader", "smt4"), note.getJSONArray("tags").let { a -> List(a.length()) { a.getString(it) } })
        // Anki is told to look for the word where this side looks: the deck and its subdecks.
        val options = note.getJSONObject("options")
        assertFalse(options.getBoolean("allowDuplicate"))
        assertEquals("deck", options.getString("duplicateScope"))
        assertEquals("日本語::Mining", options.getJSONObject("duplicateScopeOptions").getString("deckName"))
        assertTrue(options.getJSONObject("duplicateScopeOptions").getBoolean("checkChildren"))

        anki.addNote("Default", "Lapis", mapOf("Expression" to "道"), emptyList(), Duplicates.REFUSE_ANYWHERE)
        val anywhere = requests.last().getJSONObject("params").getJSONObject("note").getJSONObject("options")
        assertFalse(anywhere.getBoolean("allowDuplicate"))
        assertFalse("without a scope Anki looks in the whole collection", anywhere.has("duplicateScope"))

        anki.addNote("Default", "Lapis", mapOf("Expression" to "道"), emptyList(), Duplicates.ALLOW)
        assertTrue(
            requests.last().getJSONObject("params").getJSONObject("note").getJSONObject("options")
                .getBoolean("allowDuplicate")
        )
    }

    private fun fieldsOf(request: org.json.JSONObject) =
        request.getJSONObject("params").getJSONObject("note").getJSONObject("fields")

    @Test
    fun aCardThroughTheMediaAddOnGetsTheGamesAddressByItself() {
        // VN Library's media add-on, the way from a handheld into Anki on a computer, refuses
        // a note without a web address in its source field. The game's own page is one.
        val url = SourceAddress.of("ULJM05000")
        assertEquals("https://www.jp.playstation.com/software/title/uljm05000.html", url)
        assertEquals("", SourceAddress.of(""))
        assertEquals("", SourceAddress.of("not a game"))
        val title = "ファタモルガーナの館 <R>"
        val anki = client()
        fun add(fields: Map<String, String>, known: String? = null) = SourceAddress.addNote(
            anki, "Default", "Lapis", fields, listOf("ppsspp-reader"), Duplicates.ALLOW, url, title, known
        )

        // Straight to AnkiConnect nothing is asked for and nothing is added to the note.
        val plain = add(mapOf("Expression" to "館", "source" to "ファタモルガーナの館"))
        assertEquals(null, plain.field)
        assertEquals(1, stub.received("addNote").size)
        assertEquals("ファタモルガーナの館", fieldsOf(requests.last()).getString("source"))

        // Through the add-on: refused once, answered with the address around the field's
        // text, added. The user set nothing up for it.
        stub.addressField = "source"
        val first = add(mapOf("Expression" to "館", "source" to "ファタモルガーナの館"))
        assertEquals(1496198395707L, first.id)
        assertEquals("source", first.field)
        assertEquals(3, stub.received("addNote").size)
        assertEquals("<a href=\"$url\">ファタモルガーナの館</a>", fieldsOf(requests.last()).getString("source"))
        assertEquals("館", fieldsOf(requests.last()).getString("Expression"))

        // Remembered, the next card is right the first time.
        val second = add(mapOf("Expression" to "扉", "source" to "ファタモルガーナの館"), known = "source")
        assertEquals("source", second.field)
        assertEquals(4, stub.received("addNote").size)

        // A field nothing was mapped to gets the game's title, as text.
        stub.addressField = "Source"
        val unmapped = add(mapOf("Expression" to "鐘"))
        assertEquals("Source", unmapped.field)
        assertEquals(
            "<a href=\"$url\">ファタモルガーナの館 &lt;R&gt;</a>",
            fieldsOf(requests.last()).getString("Source")
        )

        // A format that maps the field to {url}, as in Yomitan, needs no help.
        val before = stub.received("addNote").size
        val mapped = add(mapOf("Expression" to "鍵", "Source" to url))
        assertEquals(null, mapped.field)
        assertEquals(before + 1, stub.received("addNote").size)
        assertEquals(url, fieldsOf(requests.last()).getString("Source"))

        // Anything else Anki says is passed on, and asked once.
        val asked = stub.received("addNote").size
        val error = assertThrows(AnkiException::class.java) { add(mapOf("Expression" to "重複", "Source" to url)) }
        assertEquals("cannot create note because it is a duplicate", error.message)
        assertEquals(asked + 1, stub.received("addNote").size)
    }

    @Test
    fun theAddOnsRefusalsAreToldFromOtherErrors() {
        // The add-on's own sentences (vnkit/anki_bridge.py in VN Library), with the field's
        // name as its user configured it.
        assertEquals("Source", SourceAddress.askedFor("Map Source to {url} in Yomitan so game media can be matched safely"))
        assertEquals("source", SourceAddress.askedFor("The source field must contain Yomitan’s {url}, not the page title"))
        assertEquals("Source URL", SourceAddress.askedFor("The Source URL field must contain Yomitan's {url}, not the page title"))
        assertEquals(null, SourceAddress.askedFor("Map the Picture field in Yomitan (leave its template blank)"))
        assertEquals(null, SourceAddress.askedFor("cannot create note because it is a duplicate"))
        assertEquals(null, SourceAddress.askedFor("Connection key missing or incorrect."))
        assertEquals(null, SourceAddress.askedFor(null))

        assertTrue(SourceAddress.hasAddress("<a href=\"https://example.org/x\">y</a>"))
        assertFalse(SourceAddress.hasAddress("ファタモルガーナの館"))
        // Without a game there is no address to give: the note is left as it is.
        val fields = mapOf("Source" to "x")
        assertTrue(SourceAddress.with(fields, "Source", "", "T") === fields)
    }

    @Test
    fun findsTheNotesOfAWordAndShowsThem() {
        val anki = client()
        stub.find = { query -> if (query.contains("見張る")) listOf(1700000000001L, 1700000000002L) else emptyList() }
        assertEquals(
            listOf(1700000000001L, 1700000000002L),
            anki.findNotes("日本語::Mining", "Expression", "見張る")
        )
        assertEquals(
            "\"deck:日本語::Mining\" \"Expression:見張る\"",
            requests.last().getJSONObject("params").getString("query")
        )
        assertEquals(emptyList<Long>(), anki.findNotes(null, "Expression", "道"))
        assertEquals("\"Expression:道\"", requests.last().getJSONObject("params").getString("query"))

        anki.showNotes(listOf(1700000000001L, 1700000000002L))
        assertEquals("guiBrowse", requests.last().getString("action"))
        assertEquals("nid:1700000000001,1700000000002", requests.last().getJSONObject("params").getString("query"))
        val before = requests.size
        anki.showNotes(emptyList())
        assertEquals("nothing to show, nothing asked", before, requests.size)
    }

    @Test
    fun searchesTakeNamesAndWordsLiterally() {
        // In Anki's search a quote ends the term, and * and _ stand for any text.
        assertEquals("a\\\"b\\\\c\\*d\\_e", AnkiSearch.escape("a\"b\\c*d_e"))
        assertEquals(
            "\"deck:My\\_Deck\" \"Word\\:1:5\\*\"",
            AnkiSearch.fieldIs("My_Deck", "Word:1", "5*")
        )
        // Colons in the value need nothing: the first one already ended the field's name.
        assertEquals("\"Front:a:b\"", AnkiSearch.fieldIs(null, "Front", "a:b"))
    }

    @Test
    fun reportsAnkisOwnErrors() {
        val duplicate = assertThrows(AnkiException::class.java) {
            client().addNote("Default", "Basic", mapOf("Expression" to "重複"), emptyList(), Duplicates.REFUSE_IN_DECK)
        }
        assertTrue(duplicate.message!!.contains("duplicate"))
    }

    @Test
    fun sendsTheApiKeyOnlyInTheRequestBody() {
        stub.requiredKey = "secret"
        val refused = assertThrows(AnkiException::class.java) { client().deckNames() }
        assertTrue(refused.message!!.contains("api key"))
        assertEquals(listOf("Default", "日本語::Mining"), client("secret").deckNames())
        assertEquals("secret", requests.last().getString("key"))
    }

    @Test
    fun explainsUnreachableAndBrokenServers() {
        val port = stub.port
        stub.close()
        val unreachable = assertThrows(AnkiException::class.java) {
            AnkiConnectClient("http://127.0.0.1:$port").deckNames()
        }
        assertTrue(unreachable.message!!.contains("Cannot reach AnkiConnect"))
        stub = AnkiConnectStub()
        stub.status = 500
        val failed = assertThrows(AnkiException::class.java) { client().deckNames() }
        assertTrue(failed.message!!, failed.message!!.contains("500"))
    }
}
