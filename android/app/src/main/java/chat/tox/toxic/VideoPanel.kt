// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.content.Context
import android.graphics.SurfaceTexture
import android.view.Gravity
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.FrameLayout
import kotlin.math.hypot

/**
 * Shows a video call over the terminal, with a small preview of our camera in the corner. Drag it
 * out of the way, or tap it to make it big or small.
 */
class VideoPanel(context: Context) : FrameLayout(context) {
    private val receive = videoView(Calls.RECEIVE)
    private val preview = videoView(Calls.PREVIEW)
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var big = false
    private var downX = 0f
    private var downY = 0f
    private var startX = 0f
    private var startY = 0f
    private var dragged = false

    init {
        setBackgroundColor(0xff000000.toInt())
        elevation = 8f
        addView(receive)
        addView(preview)
        visibility = GONE
    }

    private fun videoView(window: Int) = TextureView(context).apply {
        surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            private var surface: Surface? = null

            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                surface = Surface(texture).also { Terminal.setVideoSurface(window, it) }
            }

            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                Terminal.setVideoSurface(window, null)
                surface?.release()
                return true
            }

            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}

            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
        }
    }

    /** Shows the video windows toxic has open, sized to fit the terminal area. */
    fun update(calls: Calls, areaWidth: Int, areaHeight: Int) {
        val receiving = calls.isShown(Calls.RECEIVE)
        val sending = calls.isShown(Calls.PREVIEW)
        visibility = if (receiving || sending) VISIBLE else GONE

        if (!receiving && !sending) {
            big = false
            return
        }

        // Without received video, our preview takes the whole panel.
        val aspect = aspect(calls, if (receiving) Calls.RECEIVE else Calls.PREVIEW)
        var width = (areaWidth * (if (big) 1f else SMALL)).toInt()
        var height = (width / aspect).toInt()

        if (height > areaHeight) {
            height = areaHeight
            width = (height * aspect).toInt()
        }

        layoutParams = LayoutParams(width, height, Gravity.TOP or Gravity.END)
        receive.visibility = if (receiving) VISIBLE else GONE
        receive.layoutParams = LayoutParams(MATCH_PARENT, MATCH_PARENT)

        val previewWidth = if (receiving) (width * PREVIEW).toInt() else width
        val previewHeight = if (receiving) (previewWidth / aspect(calls, Calls.PREVIEW)).toInt() else height
        preview.visibility = if (sending) VISIBLE else GONE
        preview.layoutParams = LayoutParams(previewWidth, previewHeight, Gravity.BOTTOM or Gravity.END)

        if (big) {
            translationX = 0f
            translationY = 0f
        }
    }

    override fun onInterceptTouchEvent(event: MotionEvent) = true

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val parent = parent as View

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.rawX
                downY = event.rawY
                startX = translationX
                startY = translationY
                dragged = false
            }

            MotionEvent.ACTION_MOVE -> {
                val dx = event.rawX - downX
                val dy = event.rawY - downY
                dragged = dragged || hypot(dx, dy) > touchSlop

                if (dragged && !big) {
                    // Keep it inside the terminal. It starts at the top right.
                    translationX = maxOf((width - parent.width).toFloat(), minOf(0f, startX + dx))
                    translationY = maxOf(0f, minOf((parent.height - height).toFloat(), startY + dy))
                }
            }

            MotionEvent.ACTION_UP -> if (!dragged) {
                big = !big
                update(Calls.get(context), parent.width, parent.height)
            }
        }

        return true
    }

    private companion object {
        /** Fractions: SMALL of the terminal's width, PREVIEW of the panel's. */
        const val SMALL = 0.45f
        const val PREVIEW = 0.3f
        const val DEFAULT_ASPECT = 3f / 4f

        fun aspect(calls: Calls, window: Int) =
            if (calls.width(window) > 0 && calls.height(window) > 0) {
                calls.width(window).toFloat() / calls.height(window)
            } else {
                DEFAULT_ASPECT
            }
    }
}
