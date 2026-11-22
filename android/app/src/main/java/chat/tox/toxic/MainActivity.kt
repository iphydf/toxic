// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.LinearLayout.LayoutParams.MATCH_PARENT
import android.widget.LinearLayout.LayoutParams.WRAP_CONTENT

/** Shows the terminal with a row of keys that phone keyboards don't have. */
class MainActivity : Activity() {
    private lateinit var terminal: TerminalView
    private lateinit var terminalArea: FrameLayout
    private lateinit var videoPanel: VideoPanel
    private lateinit var callBar: View
    private lateinit var speakerKey: Button
    private var ctrlLatched = false

    private val calls get() = Calls.get(this)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // This must run before the terminal starts toxic.
        Certificates.export(cacheDir)
        terminal = TerminalView(this, Startup.load(this).args())
        videoPanel = VideoPanel(this)
        terminalArea = FrameLayout(this).apply {
            addView(terminal)
            addView(videoPanel)
            addOnLayoutChangeListener { view, l, t, r, b, ol, ot, or, ob ->
                if (r - l != or - ol || b - t != ob - ot) {
                    view.post(::onCallChanged)
                }
            }
        }
        callBar = callKeys()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xff000000.toInt())
            addView(terminalArea, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(callBar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(extraKeys(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        setContentView(root)
        calls.listener = Calls.Listener(::onCallChanged)
        onCallChanged()

        root.keepClearOfBars()
        requestMissingPermissions()
        startService()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val step = textSizeStep(keyCode)

        if (step == 0) {
            return super.onKeyDown(keyCode, event)
        }

        terminal.stepTextSize(step)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent) =
        textSizeStep(keyCode) != 0 || super.onKeyUp(keyCode, event)

    /** The volume keys change the text size, except in calls: toxic makes no sound otherwise. */
    private fun textSizeStep(keyCode: Int) = when {
        calls.inCall -> 0
        keyCode == KeyEvent.KEYCODE_VOLUME_UP -> 1
        keyCode == KeyEvent.KEYCODE_VOLUME_DOWN -> -1
        else -> 0
    }

    override fun onDestroy() {
        calls.listener = null
        super.onDestroy()
    }

    private fun requestMissingPermissions() {
        val missing = listOfNotNull(
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.CAMERA,
            Manifest.permission.POST_NOTIFICATIONS.takeIf { Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU },
        ).filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 0)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray) {
        // Start the service again so it can use the new permissions in the background.
        startService()
    }

    private fun startService() {
        startForegroundService(Intent(this, ToxicService::class.java))
    }

    private fun onCallChanged() {
        videoPanel.update(calls, terminalArea.width, terminalArea.height)
        callBar.visibility = if (calls.inCall) View.VISIBLE else View.GONE
        speakerKey.setTextColor(if (calls.speaker) KEY_LATCHED_FG else KEY_FG)

        // Keep the screen on while there's video to watch. In audio calls it can go dark, like in phone calls.
        if (calls.isShown(Calls.PREVIEW) || calls.isShown(Calls.RECEIVE)) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    /** toxic's call commands only work in the chat window of the friend in the call. */
    private fun callKeys() = keyRow(
        key("Speaker") { calls.speaker = !calls.speaker }.also { speakerKey = it },
        key("Mute") { terminal.command("/mute in") },
        key("Video") { terminal.command("/video") },
        key("Hang up") { terminal.command("/hangup") },
    )

    override fun onResume() {
        super.onResume()
        terminal.requestFocus()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        // Keep toxic running. Only /exit stops it.
        moveTaskToBack(true)
    }

    private fun extraKeys(): View {
        lateinit var ctrl: Button
        ctrl = key("Ctrl") {
            ctrlLatched = !ctrlLatched
            ctrl.setTextColor(if (ctrlLatched) KEY_LATCHED_FG else KEY_FG)
            terminal.setCtrlLatched(ctrlLatched) {
                ctrlLatched = false
                ctrl.setTextColor(KEY_FG)
            }
        }

        return keyRow(
            typing("Esc", "\u001b"),
            ctrl,
            typing("Tab", "\t"),
            // toxic's default keys for the previous and next window.
            typing("◀", "\u000f"),
            typing("▶", "\u0010"),
            typing("←", "\u001b[D"),
            typing("↑", "\u001b[A"),
            typing("↓", "\u001b[B"),
            typing("→", "\u001b[C"),
            typing("PgUp", "\u001b[5~"),
            typing("PgDn", "\u001b[6~"),
            key("⚙") { startActivity(Intent(this, SettingsActivity::class.java)) },
        )
    }

    private fun keyRow(vararg keys: Button) = LinearLayout(this).apply {
        setBackgroundColor(KEYS_BG)
        gravity = Gravity.CENTER_VERTICAL

        for (key in keys) {
            addView(key, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
    }

    private fun typing(label: String, sequence: String) = key(label) { terminal.type(sequence) }

    private fun key(label: String, onClick: () -> Unit) =
        Button(this, null, android.R.attr.borderlessButtonStyle).apply {
            text = label
            isAllCaps = false
            setTextColor(KEY_FG)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            minWidth = 0
            minimumWidth = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { onClick() }
        }

    private companion object {
        const val KEYS_BG = 0xff202020.toInt()
        const val KEY_FG = 0xffd0d0d0.toInt()
        const val KEY_LATCHED_FG = 0xff6fbf6f.toInt()
    }
}
