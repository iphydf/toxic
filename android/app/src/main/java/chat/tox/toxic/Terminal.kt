// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.view.Surface

/** toxic runs in this process on its own thread, behind a terminal (session.c). */
object Terminal {
    init {
        System.loadLibrary("toxic")
    }

    /** Placeholder in the cell after a double-width character (VTERM_WIDE_TAIL). */
    const val WIDE_TAIL = -1

    /** A cell holds a character and up to 4 combining ones after it (VTERM_CELL_CHARS). */
    const val CELL_CHARS = 5

    /** A cell's look is its foreground and background as ARGB, then its style (Vterm_Look). */
    const val LOOK_INTS = 3

    // Style flags, as VTERM_BOLD, VTERM_ITALIC and VTERM_UNDERLINE in vterm.h.
    const val BOLD = 1 shl 18
    const val ITALIC = 1 shl 20
    const val UNDERLINE = 1 shl 21

    fun interface Listener {
        /** Called on the terminal's thread. */
        fun onScreenChanged()
    }

    interface CallListener {
        /** A video window (Calls.PREVIEW or Calls.RECEIVE) opened or closed. Called on toxic's threads. */
        fun onVideoWindow(window: Int, shown: Boolean, width: Int, height: Int)

        /** toxic started or stopped using the microphone for a call. Called on toxic's threads. */
        fun onCallAudio(on: Boolean)
    }

    @Volatile
    var listener: Listener? = null

    @Volatile
    var callListener: CallListener? = null

    // Called from session.c. These must return void, or JNI won't find them.

    @JvmStatic
    @Suppress("unused")
    private fun onVideoWindow(window: Int, shown: Boolean, width: Int, height: Int) {
        callListener?.onVideoWindow(window, shown, width, height)
    }

    @JvmStatic
    @Suppress("unused")
    private fun onCallAudio(on: Boolean) {
        callListener?.onCallAudio(on)
    }

    @JvmStatic
    @Suppress("unused")
    private fun onScreenChanged() {
        listener?.onScreenChanged()
    }

    /**
     * Starts toxic once per process, with its files in home and args as its command line. Later
     * calls do nothing.
     */
    @JvmStatic
    external fun start(home: String, cache: String, args: Array<String>, rows: Int, cols: Int): Boolean

    /** Whether toxic's OpenSSL can read a DER certificate. */
    @JvmStatic
    external fun readsCertificate(der: ByteArray): Boolean

    /** Types data into toxic. */
    @JvmStatic
    external fun write(data: ByteArray)

    /** Asks toxic to save its data and exit, which ends this process. */
    @JvmStatic
    external fun quit()

    @JvmStatic
    external fun resize(rows: Int, cols: Int)

    /** Sets the surface toxic draws a video window on, or null if there isn't one. */
    @JvmStatic
    external fun setVideoSurface(window: Int, surface: Surface?)

    /**
     * Copies the screen into chars (CELL_CHARS per cell, 0 after the last) and looks (LOOK_INTS per
     * cell), row by row.
     */
    @JvmStatic
    external fun snapshot(rows: Int, cols: Int, chars: IntArray, looks: IntArray)
}
