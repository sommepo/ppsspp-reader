// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

import android.content.pm.PackageManager
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import java.util.concurrent.Executors
import org.ppsspp.ppsspp.R
import org.ppsspp.ppsspp.reader.ReaderSettings
import org.ppsspp.ppsspp.reader.anki.AnkiClient
import org.ppsspp.ppsspp.reader.anki.AnkiConnectClient
import org.ppsspp.ppsspp.reader.anki.AnkiDroidClient
import org.ppsspp.ppsspp.reader.anki.CardBuilder
import org.ppsspp.ppsspp.reader.mining.Miner

/**
 * Chooses where cards go (AnkiDroid here, or Anki on a computer through AnkiConnect), the deck,
 * the note type and what each of its fields receives.
 */
class AnkiSetupActivity : ReaderScreen() {
    override val titleRes = R.string.reader_anki_setup

    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var settings: ReaderSettings

    private lateinit var targets: RadioGroup
    private lateinit var connectBox: LinearLayout
    private lateinit var urlField: EditText
    private lateinit var keyField: EditText
    private lateinit var status: TextView
    private lateinit var deckSpinner: Spinner
    private lateinit var modelSpinner: Spinner
    private lateinit var fieldsBox: LinearLayout
    private lateinit var tagsField: EditText
    private lateinit var duplicates: CheckBox
    private lateinit var anywhere: CheckBox

    private var client: AnkiClient? = null
    private var decks: List<String> = emptyList()
    private var models: List<String> = emptyList()
    private val fieldEditors = LinkedHashMap<String, EditText>()

    /** Templates per note type while the screen is open, so switching type keeps what was typed. */
    private val drafts = HashMap<String, Map<String, String>>()
    private var shownModel: String? = null

    private val askPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) connect() else status.setText(R.string.reader_ankidroid_permission)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = ReaderSettings(this)
        val column = scrollingColumn()

        column.addView(heading(getString(R.string.reader_anki_target)))
        targets = RadioGroup(this)
        targets.addView(radio(ID_ANKIDROID, R.string.reader_anki_target_ankidroid))
        targets.addView(radio(ID_ANKICONNECT, R.string.reader_anki_target_ankiconnect))
        targets.addView(radio(ID_NONE, R.string.reader_anki_target_none))
        column.addView(targets)

        connectBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        urlField = field(R.string.reader_anki_url_hint, InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI)
        keyField = field(0, InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        connectBox.addView(note(getString(R.string.reader_anki_url)))
        connectBox.addView(urlField)
        connectBox.addView(note(getString(R.string.reader_anki_key)))
        connectBox.addView(keyField)
        column.addView(connectBox)

        column.addView(
            Button(this).apply {
                setText(R.string.reader_anki_connect)
                isAllCaps = false
                setOnClickListener { connect() }
            },
            LinearLayout.LayoutParams(WRAP, WRAP)
        )
        status = note("").apply { setPadding(0, dp(4f), 0, dp(4f)) }
        column.addView(status)

        column.addView(heading(getString(R.string.reader_anki_deck)))
        deckSpinner = Spinner(this)
        column.addView(deckSpinner)
        column.addView(heading(getString(R.string.reader_anki_model)))
        modelSpinner = Spinner(this)
        column.addView(modelSpinner)

        column.addView(heading(getString(R.string.reader_anki_fields)))
        column.addView(note(getString(R.string.reader_anki_fields_help)))
        column.addView(
            Button(this, null, android.R.attr.borderlessButtonStyle).apply {
                setText(R.string.reader_anki_suggest)
                isAllCaps = false
                setOnClickListener { suggest() }
            },
            LinearLayout.LayoutParams(WRAP, WRAP)
        )
        fieldsBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(fieldsBox)

        column.addView(heading(getString(R.string.reader_anki_tags)))
        tagsField = field(0, InputType.TYPE_CLASS_TEXT)
        column.addView(tagsField)
        duplicates = CheckBox(this).apply { setText(R.string.reader_anki_allow_duplicates) }
        column.addView(duplicates)
        anywhere = CheckBox(this).apply { setText(R.string.reader_anki_duplicates_anywhere) }
        column.addView(anywhere)

        column.addView(spacer(12f))
        column.addView(
            Button(this).apply {
                setText(R.string.reader_anki_save)
                isAllCaps = false
                setOnClickListener { save() }
            },
            LinearLayout.LayoutParams(WRAP, WRAP)
        )

        restore()
        targets.setOnCheckedChangeListener { _, _ -> onTargetChanged() }
        onTargetChanged()
        // Connect right away where that takes nothing from the user. Asking for access to
        // AnkiDroid is their move: it waits for the Connect button.
        connect(mayAsk = false)
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    private fun radio(id: Int, label: Int) = RadioButton(this).apply {
        this.id = id
        setText(label)
    }

    private fun field(hint: Int, type: Int) = EditText(this).apply {
        if (hint != 0) setHint(hint)
        inputType = type
        isSingleLine = true
    }

    private fun currentTarget(): String = when (targets.checkedRadioButtonId) {
        ID_ANKIDROID -> ReaderSettings.ANKI_ANKIDROID
        ID_ANKICONNECT -> ReaderSettings.ANKI_ANKICONNECT
        else -> ReaderSettings.ANKI_NONE
    }

    private fun restore() {
        targets.check(
            when (settings.ankiTransport) {
                ReaderSettings.ANKI_ANKICONNECT -> ID_ANKICONNECT
                ReaderSettings.ANKI_NONE -> ID_NONE
                else -> ID_ANKIDROID
            }
        )
        urlField.setText(settings.ankiConnectUrl)
        keyField.setText(settings.ankiConnectKey)
        tagsField.setText(settings.ankiTags)
        duplicates.isChecked = settings.ankiAllowDuplicates
        anywhere.isChecked = settings.ankiDuplicatesAnywhere
        if (settings.ankiModel.isNotEmpty()) {
            drafts[settings.ankiModel] = CardBuilder.parseMapping(settings.ankiFieldMap)
        }
    }

    private fun onTargetChanged() {
        connectBox.visibility = if (currentTarget() == ReaderSettings.ANKI_ANKICONNECT) View.VISIBLE else View.GONE
    }

    // ---- Connecting ----

    /**
     * Connects to where cards go. With [mayAsk], AnkiDroid's permission dialog is shown if
     * access was not given yet; without it, the status line says to press Connect.
     */
    private fun connect(mayAsk: Boolean = true) {
        val target = currentTarget()
        client = null
        if (mayAsk) rememberTarget()
        when (target) {
            ReaderSettings.ANKI_NONE -> {
                status.text = ""
                return
            }

            ReaderSettings.ANKI_ANKIDROID -> {
                if (!AnkiDroidClient.isInstalled(this)) {
                    status.setText(R.string.reader_ankidroid_missing)
                    return
                }
                if (checkSelfPermission(AnkiDroidClient.PERMISSION) != PackageManager.PERMISSION_GRANTED) {
                    if (mayAsk) {
                        askPermission.launch(AnkiDroidClient.PERMISSION)
                    } else {
                        status.setText(R.string.reader_ankidroid_permission)
                    }
                    return
                }
            }
        }
        val candidate: AnkiClient = if (target == ReaderSettings.ANKI_ANKIDROID) {
            AnkiDroidClient(applicationContext)
        } else {
            val url = urlField.text.toString().trim()
            if (url.isEmpty()) {
                if (mayAsk) status.setText(R.string.reader_anki_url)
                return
            }
            AnkiConnectClient(url, keyField.text.toString())
        }
        status.text = "…"
        worker.execute {
            val result = runCatching { candidate.deckNames() to candidate.modelNames() }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                result.onSuccess { (deckNames, modelNames) ->
                    client = candidate
                    decks = deckNames
                    models = modelNames
                    status.text = getString(R.string.reader_anki_connected, decks.size, models.size)
                    fillSpinners()
                }.onFailure {
                    status.text = getString(R.string.reader_anki_connect_failed, it.message ?: "")
                }
            }
        }
    }

    /**
     * Keeps the choice of destination and its address as soon as Connect is pressed: it should
     * not be lost because the connection failed or the screen was left without saving.
     */
    private fun rememberTarget() {
        settings.preferences.edit()
            .putString(ReaderSettings.KEY_ANKI_TRANSPORT, currentTarget())
            .putString(ReaderSettings.KEY_ANKICONNECT_URL, urlField.text.toString().trim())
            .putString(ReaderSettings.KEY_ANKICONNECT_KEY, keyField.text.toString())
            .apply()
    }

    private fun fillSpinners() {
        deckSpinner.adapter = adapter(decks)
        decks.indexOf(settings.ankiDeck).takeIf { it >= 0 }?.let(deckSpinner::setSelection)
        modelSpinner.adapter = adapter(models)
        modelSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                loadFields(models[position])
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
        models.indexOf(settings.ankiModel).takeIf { it >= 0 }?.let(modelSpinner::setSelection)
    }

    private fun adapter(items: List<String>) =
        ArrayAdapter(this, android.R.layout.simple_spinner_item, items).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }

    // ---- Fields ----

    private fun loadFields(model: String) {
        val anki = client ?: return
        rememberDraft()
        worker.execute {
            val result = runCatching { anki.fieldNames(model) }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                result.onSuccess { showFields(model, it) }
                    .onFailure { status.text = getString(R.string.reader_anki_connect_failed, it.message ?: "") }
            }
        }
    }

    private fun showFields(model: String, fields: List<String>) {
        shownModel = model
        val templates = drafts[model] ?: CardBuilder.suggestMapping(fields)
        fieldsBox.removeAllViews()
        fieldEditors.clear()
        for (name in fields) {
            fieldsBox.addView(note(name).apply { setPadding(0, dp(8f), 0, 0) })
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            val editor = field(0, InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS)
            editor.setText(templates[name] ?: "")
            row.addView(editor, LinearLayout.LayoutParams(0, WRAP, 1f))
            row.addView(
                ImageButton(this, null, android.R.attr.borderlessButtonStyle).apply {
                    setImageResource(R.drawable.ic_reader_x_add)
                    contentDescription = getString(R.string.reader_anki_marker_pick)
                    setOnClickListener { pickMarker(it, editor) }
                },
                LinearLayout.LayoutParams(dp(44f), dp(44f))
            )
            fieldsBox.addView(row)
            fieldEditors[name] = editor
        }
    }

    private fun pickMarker(anchor: View, editor: EditText) {
        val menu = PopupMenu(this, anchor)
        CardBuilder.MARKERS.forEachIndexed { index, (marker, description) ->
            menu.menu.add(0, index, index, "{$marker}  $description")
        }
        menu.setOnMenuItemClickListener { item ->
            val marker = "{${CardBuilder.MARKERS[item.itemId].first}}"
            val at = editor.selectionStart.coerceAtLeast(0)
            editor.text.insert(at, marker)
            true
        }
        menu.show()
    }

    private fun rememberDraft() {
        val model = shownModel ?: return
        drafts[model] = fieldEditors.mapValues { it.value.text.toString() }
    }

    private fun suggest() {
        val suggestion = CardBuilder.suggestMapping(fieldEditors.keys.toList())
        for ((name, editor) in fieldEditors) editor.setText(suggestion[name] ?: "")
    }

    private fun save() {
        rememberDraft()
        val editor = settings.preferences.edit()
        editor.putString(ReaderSettings.KEY_ANKI_TRANSPORT, currentTarget())
        editor.putString(ReaderSettings.KEY_ANKICONNECT_URL, urlField.text.toString().trim())
        editor.putString(ReaderSettings.KEY_ANKICONNECT_KEY, keyField.text.toString())
        editor.putString(ReaderSettings.KEY_ANKI_TAGS, tagsField.text.toString().trim())
        editor.putBoolean(ReaderSettings.KEY_ANKI_DUPLICATES, duplicates.isChecked)
        editor.putBoolean(ReaderSettings.KEY_ANKI_DUPLICATES_ANYWHERE, anywhere.isChecked)
        (deckSpinner.selectedItem as? String)?.let { editor.putString(ReaderSettings.KEY_ANKI_DECK, it) }
        val model = shownModel
        if (model != null) {
            editor.putString(ReaderSettings.KEY_ANKI_MODEL, model)
            editor.putString(
                ReaderSettings.KEY_ANKI_FIELDS,
                CardBuilder.mappingToJson(drafts[model].orEmpty())
            )
            // Anki tells notes apart by their first field; that is what "already there" goes by.
            fieldEditors.keys.firstOrNull()?.let { first ->
                editor.putString(ReaderSettings.KEY_ANKI_KEY_FIELD, ReaderSettings.keyFieldValue(model, first))
            }
        }
        editor.apply()
        Miner.forget()
        Toast.makeText(this, R.string.reader_anki_saved, Toast.LENGTH_SHORT).show()
        finish()
    }

    private companion object {
        val ID_ANKIDROID = R.id.reader_anki_target_ankidroid
        val ID_ANKICONNECT = R.id.reader_anki_target_ankiconnect
        val ID_NONE = R.id.reader_anki_target_none
    }
}
