// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.view.Gravity
import android.widget.TextView
import java.io.File
import kotlin.concurrent.thread

/**
 * Starts toxic again. toxic can only start once per process, so this runs in its own process
 * (":restart"), waits for toxic's process to end and then starts the app in a new one. It's an
 * activity because Android only lets apps on the screen start activities.
 */
class RestartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(TextView(this).apply {
            text = "Restarting toxic…"
            gravity = Gravity.CENTER
        })

        val pid = intent.getIntExtra(EXTRA_PID, -1)
        thread(name = "restart") {
            awaitExit(pid)
            runOnUiThread {
                startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                finish()
            }
        }
    }

    companion object {
        private const val EXTRA_PID = "chat.tox.toxic.PID"

        /** How long toxic gets to save and exit before its process is killed. */
        private const val EXIT_TIMEOUT_MS = 5000L

        /** Call this from toxic's process. toxic saves its data and exits. */
        fun restart(context: Context) {
            context.startActivity(Intent(context, RestartActivity::class.java).putExtra(EXTRA_PID, Process.myPid()))
            Terminal.quit()
        }

        private fun awaitExit(pid: Int) {
            val process = File("/proc/$pid")
            val deadline = SystemClock.elapsedRealtime() + EXIT_TIMEOUT_MS

            while (pid > 0 && process.exists() && SystemClock.elapsedRealtime() < deadline + EXIT_TIMEOUT_MS) {
                if (SystemClock.elapsedRealtime() > deadline) {
                    Process.killProcess(pid)
                }

                SystemClock.sleep(50)
            }
        }
    }
}
