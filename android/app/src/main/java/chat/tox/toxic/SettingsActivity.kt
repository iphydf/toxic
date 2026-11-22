// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.LinearLayout.LayoutParams.MATCH_PARENT
import android.widget.LinearLayout.LayoutParams.WRAP_CONTENT
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import android.widget.Toolbar
import java.io.File
import java.io.IOException

/**
 * Shows toxic's command-line options and the settings in toxic.conf, laid out like toxic's example
 * config (ConfigSchema). We only write settings that aren't toxic's default to toxic.conf, and
 * toxic reloads it after a save.
 */
class SettingsActivity : Activity() {
    /** A toxic.conf setting on the screen. */
    private class Field(
        val option: ConfigSchema.Option,
        val value: () -> String,
        val show: (String) -> Unit,
        /** Where to show an invalid value. Null for switches and drop-downs. */
        val input: TextView?,
    )

    private val fields = mutableListOf<Field>()
    private lateinit var startup: Startup
    private lateinit var udp: Switch
    private lateinit var lanDiscovery: Switch
    private lateinit var proxy: Spinner
    private lateinit var proxyHost: EditText
    private lateinit var proxyPort: EditText
    private lateinit var proxyHostRow: View
    private lateinit var proxyPortRow: View

    /** toxic.conf as it is in the file, and its values by path. */
    private var configText = ""
    private var configValues = mapOf<String, String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        startup = Startup.load(this)

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(24))
        }
        startupSettings(content)
        configSettings(content)

        val toolbar = Toolbar(this).apply {
            setBackgroundColor(TOOLBAR_BG)
            title = "Settings"
            setTitleTextColor(0xffffffff.toInt())
        }
        setActionBar(toolbar)

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(TOOLBAR_BG)
            addView(toolbar)
            addView(ScrollView(context).apply { addView(content) }, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            keepClearOfBars()
        })

        // Start at the top, not at the first text field.
        content.isFocusableInTouchMode = true
        content.requestFocus()
    }

    private fun startupSettings(content: LinearLayout) {
        content.addView(header("STARTUP", "toxic reads these when it starts."))

        udp = Switch(this).apply { isChecked = startup.udp }
        content.addView(switchRow("UDP", "Connect to friends directly. Off: only through TCP relays.", udp))

        lanDiscovery = Switch(this).apply { isChecked = startup.lanDiscovery }
        content.addView(switchRow("LAN discovery", "Find friends on the same network.", lanDiscovery))

        proxy = spinner(PROXY_NAMES, PROXY_NAMES[PROXIES.indexOf(startup.proxy).coerceAtLeast(0)])
        content.addView(row("Proxy", "Connect through a proxy. This turns UDP off.", proxy))
        proxyHost = text(startup.proxyHost, InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI)
        proxyHostRow = row("Proxy host", "", proxyHost).also { content.addView(it) }
        proxyPort = text(if (startup.proxyPort > 0) startup.proxyPort.toString() else "", InputType.TYPE_CLASS_NUMBER)
        proxyPortRow = row("Proxy port", "", proxyPort).also { content.addView(it) }

        proxy.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) = showProxy()

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        showProxy()

        content.addView(button("Save and restart toxic") {
            if (save()) {
                RestartActivity.restart(this)
            }
        })
    }

    private fun showProxy() {
        val proxied = proxy.selectedItemPosition > 0
        udp.isEnabled = !proxied
        proxyHostRow.visibility = if (proxied) View.VISIBLE else View.GONE
        proxyPortRow.visibility = if (proxied) View.VISIBLE else View.GONE
    }

    private fun configSettings(content: LinearLayout) {
        val sections = try {
            val schema = ConfigSchema.read(assets.open(EXAMPLE).bufferedReader().use { it.readText() })
            configText = ToxicConfig.read(ToxicConfig.file(this))
            readValues()
            schema
        } catch (e: IOException) {
            content.addView(errorText("Can't read toxic.conf: ${e.message}"))
            return
        } catch (e: IllegalArgumentException) {
            content.addView(errorText("toxic.conf has a mistake (${e.message}), so it can't be shown here."))
            content.addView(button("Start toxic.conf again", ::startConfigAgain))
            content.addView(description("The old one stays as toxic.conf.bak."))
            return
        }

        for (section in sections) {
            content.addView(header(section.name.uppercase(), section.description))

            for (option in section.options) {
                content.addView(setting(option))
            }
        }
    }

    private fun readValues() {
        configValues = ToxicConfig.entries(configText).filter { it.type != ToxicConfig.GROUP }
            .associate { it.path to it.value }
    }

    private fun startConfigAgain() {
        val file = ToxicConfig.file(this)

        if (!file.renameTo(File(file.path + ".bak"))) {
            Toast.makeText(this, "Can't move toxic.conf", Toast.LENGTH_LONG).show()
            return
        }

        recreate()
    }

    /** Long-pressing the row shows toxic's default again. */
    private fun setting(option: ConfigSchema.Option): View {
        val current = configValues[option.path] ?: option.defaultValue
        val title = option.name.replace('_', ' ')
        val (row, field) = when {
            option.type == ToxicConfig.BOOL -> {
                val input = Switch(this).apply { isChecked = current == "true" }
                switchRow(title, option.description, input) to
                    Field(option, { input.isChecked.toString() }, { input.isChecked = it == "true" }, null)
            }

            option.choices.isNotEmpty() -> {
                val choices = if (current in option.choices) option.choices else option.choices + current
                val input = spinner(choices, current)
                row(title, option.description, input) to Field(option, { input.selectedItem as String },
                    { input.setSelection(choices.indexOf(it).coerceAtLeast(0)) }, null)
            }

            else -> {
                val input = text(current, inputType(option.type)).apply { option.hint?.let { hint = it } }
                row(title, option.description, input) to
                    Field(option, { input.text.toString() }, { input.setText(it) }, input)
            }
        }

        fields += field
        row.setOnLongClickListener {
            field.show(option.defaultValue)
            Toast.makeText(this, "toxic's default", Toast.LENGTH_SHORT).show()
            true
        }
        return row
    }

    /** Returns the startup options on the screen, or null (showing why) if they aren't valid. */
    private fun screenStartup(): Startup? {
        val screen = Startup(
            udp = udp.isChecked,
            lanDiscovery = lanDiscovery.isChecked,
            proxy = PROXIES[proxy.selectedItemPosition],
            proxyHost = proxyHost.text.toString().trim(),
            proxyPort = proxyPort.text.toString().trim().toIntOrNull() ?: 0,
        )

        if (screen.proxied && screen.proxyHost.isEmpty()) {
            proxyHost.error = "A proxy needs a host"
            return null
        }

        if (screen.proxied && !Startup.validPort(screen.proxyPort)) {
            proxyPort.error = "1 to 65535"
            return null
        }

        return screen
    }

    /** Returns the changes that make toxic.conf match the screen, or null (showing why) if a value isn't valid. */
    private fun configChanges(): List<ToxicConfig.Change>? {
        val changes = mutableListOf<ToxicConfig.Change>()

        for (field in fields) {
            val option = field.option
            val value = field.value()

            if (!valid(option.type, value)) {
                field.input?.apply {
                    error = "Not a number"
                    requestFocus()
                }
                return null
            }

            val saved = configValues[option.path]

            if (same(option.type, value, option.defaultValue)) {
                if (saved != null) {
                    changes += ToxicConfig.Change(option.path, ToxicConfig.REMOVE, "")
                }
            } else if (saved == null || !same(option.type, value, saved)) {
                changes += ToxicConfig.Change(option.path, option.type, value)
            }
        }

        return changes
    }

    /** Saves the screen. Returns false, showing why, if something isn't valid or can't be saved. */
    private fun save(): Boolean {
        val screen = screenStartup()
        val changes = configChanges()

        if (screen == null || changes == null) {
            return false
        }

        screen.save(this)
        startup = screen

        if (changes.isEmpty()) {
            return true
        }

        try {
            val text = ToxicConfig.update(configText, changes)
            ToxicConfig.write(ToxicConfig.file(this), text)
            configText = text
            readValues()
        } catch (e: IOException) {
            return cantSave(e)
        } catch (e: IllegalArgumentException) {
            return cantSave(e)
        }

        Terminal.write(byteArrayOf(RELOAD_CONFIG))
        return true
    }

    private fun cantSave(e: Exception): Boolean {
        Toast.makeText(this, "Can't save toxic.conf: ${e.message}", Toast.LENGTH_LONG).show()
        return false
    }

    private fun changed(): Boolean {
        val screen = screenStartup()
        val changes = configChanges()
        return screen == null || changes == null || changes.isNotEmpty() || screen != startup
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menu.add("Save").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (save()) {
            finish()
        }

        return true
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (!changed()) {
            finish()
            return
        }

        AlertDialog.Builder(this)
            .setMessage("Save your changes?")
            .setPositiveButton("Save") { _, _ ->
                if (save()) {
                    finish()
                }
            }
            .setNegativeButton("Discard") { _, _ -> finish() }
            .show()
    }

    private fun header(title: String, description: String) = TextView(this).apply {
        text = if (description.isEmpty()) title else "$title\n$description"
        setTextColor(HEADER_FG)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        setPadding(0, dp(24), 0, dp(4))
    }

    private fun description(text: String) = TextView(this).apply {
        this.text = text
        setTextColor(DESCRIPTION_FG)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
    }

    private fun errorText(text: String) = description(text).apply { setTextColor(ERROR_FG) }

    private fun title(text: String) = TextView(this).apply {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        setOnClickListener { onClick() }
    }

    private fun titled(name: String, about: String) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        addView(title(name))

        if (about.isNotEmpty()) {
            addView(description(about))
        }
    }

    /** A title and description above a control. */
    private fun row(title: String, description: String, control: View) = titled(title, description).apply {
        setPadding(0, dp(8), 0, dp(8))
        addView(control)
    }

    /** A title and description beside a switch. */
    private fun switchRow(title: String, description: String, control: Switch) = LinearLayout(this).apply {
        setPadding(0, dp(8), 0, dp(8))
        addView(titled(title, description), LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addView(control)
        setOnClickListener { control.toggle() }
    }

    private fun spinner(choices: List<String>, selected: String) = Spinner(this).apply {
        adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, choices)
        setSelection(choices.indexOf(selected).coerceAtLeast(0))
    }

    private fun text(value: String, inputType: Int) = EditText(this).apply {
        setText(value)
        this.inputType = inputType
        isSingleLine = true
        setHintTextColor(HINT_FG)
    }

    private companion object {
        const val EXAMPLE = "toxic.conf.example"

        /** toxic's default key for reloading its config (keys.reload_config): Ctrl+R. */
        const val RELOAD_CONFIG: Byte = 0x12

        const val HEADER_FG = 0xff6fbf6f.toInt()
        const val DESCRIPTION_FG = 0xff9e9e9e.toInt()
        const val ERROR_FG = 0xffef5350.toInt()
        const val HINT_FG = 0x61ffffff
        const val TOOLBAR_BG = 0xff212121.toInt()

        val PROXIES = listOf(Startup.NO_PROXY, Startup.SOCKS5, Startup.HTTP)
        val PROXY_NAMES = listOf("None", "SOCKS5", "HTTP")

        fun inputType(type: Int) = when (type) {
            ToxicConfig.INT, ToxicConfig.INT64 -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            ToxicConfig.FLOAT -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
            else -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }

        fun valid(type: Int, value: String) = when (type) {
            ToxicConfig.INT -> value.toIntOrNull() != null
            ToxicConfig.INT64 -> value.toLongOrNull() != null
            ToxicConfig.FLOAT -> value.toDoubleOrNull() != null
            else -> true
        }

        /** Floats compare as numbers: "5" and "5.0" are the same. */
        fun same(type: Int, a: String, b: String) =
            if (type == ToxicConfig.FLOAT) a.toDouble() == b.toDouble() else a == b
    }
}
