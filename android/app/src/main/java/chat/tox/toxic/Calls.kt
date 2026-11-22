// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper

/** Sets up the phone's audio for toxic's calls and keeps track of the video windows toxic has open. */
class Calls private constructor(context: Context) : Terminal.CallListener {
    fun interface Listener {
        /** Called on the main thread. */
        fun onCallChanged()
    }

    private val audio = context.getSystemService(AudioManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private val shown = BooleanArray(2)
    private val widths = IntArray(2)
    private val heights = IntArray(2)

    var listener: Listener? = null

    var inCall = false
        private set

    var speaker = true
        set(on) {
            field = on
            if (inCall) route()
            changed()
        }

    fun isShown(window: Int) = shown[window]

    /** Returns 0 if the size isn't known. */
    fun width(window: Int) = widths[window]

    fun height(window: Int) = heights[window]

    override fun onVideoWindow(window: Int, shown: Boolean, width: Int, height: Int) {
        main.post {
            this.shown[window] = shown
            widths[window] = if (shown) width else 0
            heights[window] = if (shown) height else 0
            changed()
        }
    }

    @Suppress("DEPRECATION")
    override fun onCallAudio(on: Boolean) {
        main.post {
            inCall = on

            if (on) {
                audio.mode = AudioManager.MODE_IN_COMMUNICATION
                route()
            } else {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    audio.clearCommunicationDevice()
                } else {
                    audio.isSpeakerphoneOn = false
                }

                audio.mode = AudioManager.MODE_NORMAL
            }

            changed()
        }
    }

    /** Plays the call on the loudspeaker, or else on the earpiece or a headset. */
    @Suppress("DEPRECATION")
    private fun route() {
        when {
            Build.VERSION.SDK_INT < Build.VERSION_CODES.S -> audio.isSpeakerphoneOn = speaker
            !speaker -> audio.clearCommunicationDevice()
            else -> audio.availableCommunicationDevices
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                ?.let(audio::setCommunicationDevice)
        }
    }

    private fun changed() = listener?.onCallChanged()

    companion object {
        /** Must match Android_Video_Window in android_video.h. */
        const val PREVIEW = 0
        const val RECEIVE = 1

        private var instance: Calls? = null

        @Synchronized
        fun get(context: Context): Calls = instance ?: Calls(context.applicationContext).also {
            instance = it
            Terminal.callListener = it
        }
    }
}
