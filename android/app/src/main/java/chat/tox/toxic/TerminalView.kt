// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.icu.text.BreakIterator
import android.icu.text.Normalizer2
import android.text.InputType
import android.text.Selection
import android.util.TypedValue
import android.view.GestureDetector
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import kotlin.math.ceil
import kotlin.math.roundToInt

/**
 * Draws toxic's terminal and types into it.
 *
 * @param args toxic's command line, if this view starts toxic.
 */
class TerminalView(context: Context, private val args: Array<String>) : View(context), Terminal.Listener {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Typeface.MONOSPACE }
    private val background = Paint()
    private val lines = Paint()
    private val sizeLabel = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE
        color = SIZE_FG
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, SIZE_TEXT_SP, resources.displayMetrics)
    }
    private val graphemes = BreakIterator.getCharacterInstance()
    private val composed = Normalizer2.getNFCInstance()
    private val rowText = StringBuilder()
    private val imm get() = context.getSystemService(InputMethodManager::class.java)
    private val preferences get() = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    private var textSizeSp = preferences.getFloat(TEXT_SIZE, DEFAULT_TEXT_SP)
    private var textSize = 0f
    private var cellWidth = 0f
    private var cellHeight = 0f
    private var baseline = 0f

    /** How far below the baseline the middle of the font's glyphs is (negative: above). */
    private var glyphMiddle = 0f
    private var rows = 0
    private var cols = 0
    private var chars = IntArray(0)
    private var looks = IntArray(0)

    /** The column of each char in rowText, and cols after the last. */
    private var colAt = IntArray(1)
    private var scrolled = 0f

    /** True for a second after the text size changes, to show the grid's size. */
    private var sizeShown = false
    private val hideSize = Runnable {
        sizeShown = false
        invalidate()
    }

    private val keyboardLine = KeyboardLine { Terminal.write(it.toByteArray()) }

    /** Whether the extra keys' Ctrl applies to the next key. */
    private var ctrlLatched = false
    private var onCtrlUsed: (() -> Unit)? = null

    private val scaleDetector = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(detector: ScaleGestureDetector): Boolean {
            setTextSizeSp(textSizeSp * detector.scaleFactor)
            return true
        }

        override fun onScaleEnd(detector: ScaleGestureDetector) = saveTextSize()
    })

    private val gestureDetector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onSingleTapUp(e: MotionEvent): Boolean {
            showKeyboard()
            return true
        }

        override fun onScroll(e1: MotionEvent?, e2: MotionEvent, dx: Float, dy: Float): Boolean {
            // Dragging down shows older messages, like scrolling a list.
            scrolled += dy

            while (scrolled <= -cellHeight) {
                type(SCROLL_UP)
                scrolled += cellHeight
            }

            while (scrolled >= cellHeight) {
                type(SCROLL_DOWN)
                scrolled -= cellHeight
            }

            return true
        }
    })

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        updateTextSize()
    }

    fun setCtrlLatched(latched: Boolean, onUsed: () -> Unit) {
        ctrlLatched = latched
        onCtrlUsed = onUsed
    }

    fun showKeyboard() {
        requestFocus()
        imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    /** Types text as if on a keyboard, with Ctrl if it's latched. */
    fun type(text: String) {
        var typed = text

        if (ctrlLatched && text.length == 1) {
            typed = withCtrl(text[0])
            ctrlLatched = false
            onCtrlUsed?.invoke()
        }

        // The keyboard's line is no longer what toxic has.
        if (!keyboardLine.isEmpty()) {
            keyboardLine.reset()
            post { imm.restartInput(this) }
        }

        Terminal.write(typed.replace('\n', '\r').toByteArray())
    }

    /** Tells the keyboard where the cursor and the word it's composing are, as text fields do. */
    private fun reportSelection() {
        val line = keyboardLine.text
        imm.updateSelection(this, Selection.getSelectionStart(line), Selection.getSelectionEnd(line),
            BaseInputConnection.getComposingSpanStart(line), BaseInputConnection.getComposingSpanEnd(line))
    }

    /**
     * Runs a toxic command in the current window. Whatever was typed is cut first (Ctrl+E Ctrl+U),
     * so Ctrl+Y brings it back.
     */
    fun command(command: String) = type("\u0005\u0015$command\r")

    /** Makes the text step sp bigger, or smaller if step is negative. */
    fun stepTextSize(step: Int) {
        setTextSizeSp(textSizeSp.roundToInt() + step.toFloat())
        saveTextSize()
    }

    private fun setTextSizeSp(sp: Float) {
        textSizeSp = sp.coerceIn(MIN_TEXT_SP, MAX_TEXT_SP)
        updateTextSize()
        sizeShown = true
        removeCallbacks(hideSize)
        postDelayed(hideSize, SIZE_SHOWN_MS)
        updateGrid()
    }

    private fun saveTextSize() = preferences.edit().putFloat(TEXT_SIZE, textSizeSp).apply()

    private fun updateTextSize() {
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, textSizeSp, resources.displayMetrics)
        paint.textSize = textSize
        val metrics = paint.fontMetrics
        cellWidth = paint.measureText("M")
        cellHeight = ceil(metrics.descent - metrics.ascent)
        baseline = -metrics.ascent
        glyphMiddle = (metrics.ascent + metrics.descent) / 2
    }

    private fun updateGrid() {
        if (width == 0 || height == 0) {
            return
        }

        val newCols = maxOf(1, (width / cellWidth).toInt())
        val newRows = maxOf(1, (height / cellHeight).toInt())

        if (newRows == rows && newCols == cols) {
            invalidate()
            return
        }

        rows = newRows
        cols = newCols
        chars = IntArray(rows * cols * Terminal.CELL_CHARS)
        looks = IntArray(rows * cols * Terminal.LOOK_INTS)
        // A character is at most 2 chars.
        colAt = IntArray(cols * Terminal.CELL_CHARS * 2 + 1)
        Terminal.start(context.filesDir.absolutePath, context.cacheDir.absolutePath, args, rows, cols)
        Terminal.resize(rows, cols)
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        Terminal.listener = this
    }

    override fun onDetachedFromWindow() {
        Terminal.listener = null
        super.onDetachedFromWindow()
    }

    override fun onScreenChanged() = postInvalidate()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = updateGrid()

    private fun isWideTail(cell: Int) = chars[cell * Terminal.CELL_CHARS] == Terminal.WIDE_TAIL

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(BACKGROUND)

        if (rows == 0) {
            return
        }

        Terminal.snapshot(rows, cols, chars, looks)

        for (row in 0 until rows) {
            drawBackgrounds(canvas, row)
            drawText(canvas, row)
        }

        if (sizeShown) {
            drawSize(canvas)
        }
    }

    /** Draws the grid's size (columns × rows) in the middle of the view. */
    private fun drawSize(canvas: Canvas) {
        val size = "$cols×$rows"
        val metrics = sizeLabel.fontMetrics
        val padding = sizeLabel.textSize / 2
        val labelWidth = sizeLabel.measureText(size)
        val x = (width - labelWidth) / 2
        val y = (height - metrics.ascent - metrics.descent) / 2
        background.color = SIZE_BG
        canvas.drawRoundRect(x - padding, y + metrics.ascent - padding, x + labelWidth + padding,
            y + metrics.descent + padding, padding, padding, background)
        canvas.drawText(size, x, y, sizeLabel)
    }

    private fun drawBackgrounds(canvas: Canvas, row: Int) {
        val y = row * cellHeight

        for (col in 0 until cols) {
            val i = row * cols + col
            val bg = looks[i * Terminal.LOOK_INTS + 1]

            if (bg != BACKGROUND && !isWideTail(i)) {
                val cells = if (col + 1 < cols && isWideTail(i + 1)) 2 else 1
                background.color = bg
                canvas.drawRect(col * cellWidth, y, (col + cells) * cellWidth, y + cellHeight, background)
            }
        }
    }

    /**
     * Draws a row one grapheme cluster at a time (accents, flags, emoji sequences), like a text field
     * does. A cluster can span several cells and is drawn in its first cell's style.
     */
    private fun drawText(canvas: Canvas, row: Int) {
        rowText.setLength(0)

        for (col in 0 until cols) {
            val first = (row * cols + col) * Terminal.CELL_CHARS
            val from = rowText.length

            for (j in first until first + Terminal.CELL_CHARS) {
                if (chars[j] == 0 || chars[j] == Terminal.WIDE_TAIL) break
                rowText.appendCodePoint(chars[j])
            }

            colAt.fill(col, from, rowText.length)
        }

        colAt[rowText.length] = cols
        val text = rowText.toString()
        graphemes.setText(text)
        val y = row * cellHeight
        var start = graphemes.first()
        var end = graphemes.next()

        while (end != BreakIterator.DONE) {
            drawCluster(canvas, text, start, end, row, y)
            start = end
            end = graphemes.next()
        }
    }

    private fun drawCluster(canvas: Canvas, text: String, start: Int, end: Int, row: Int, y: Float) {
        val col = colAt[start]
        val look = (row * cols + col) * Terminal.LOOK_INTS
        val style = looks[look + 2]

        if (end == start + 1 && text[start] == ' ' && style and Terminal.UNDERLINE == 0) {
            return
        }

        paint.color = looks[look]
        paint.isFakeBoldText = style and Terminal.BOLD != 0
        paint.textSkewX = if (style and Terminal.ITALIC != 0) -0.25f else 0f
        paint.isUnderlineText = style and Terminal.UNDERLINE != 0
        val x = col * cellWidth
        val clusterWidth = (colAt[end] - col) * cellWidth
        val arms = if (end == start + 1) boxArms(text[start]) else 0

        when {
            arms != 0 -> drawBoxLines(canvas, arms, x, y)

            // Use the composed form (NFC): the monospace font has é, but not always a lone combining accent.
            Character.charCount(text.codePointAt(start)) < end - start -> {
                val cluster = composed.normalize(text.substring(start, end))
                drawFitted(canvas, cluster, 0, cluster.length, x, y, clusterWidth)
            }

            else -> drawFitted(canvas, text, start, end, x, y, clusterWidth)
        }
    }

    /**
     * Draws box lines to the cell's edges so they join the next cell's. The monospace font has no
     * box-drawing characters, and the fallback fonts' often don't join.
     */
    private fun drawBoxLines(canvas: Canvas, arms: Int, x: Float, y: Float) {
        val thickness = maxOf(1, (cellWidth / 8).roundToInt()).toFloat()
        val left = (x + (cellWidth - thickness) / 2).roundToInt().toFloat()
        val top = (y + (cellHeight - thickness) / 2).roundToInt().toFloat()
        val right = left + thickness
        val bottom = top + thickness
        lines.color = paint.color

        if (arms and LEFT != 0) canvas.drawRect(x, top, right, bottom, lines)
        if (arms and RIGHT != 0) canvas.drawRect(left, top, x + cellWidth, bottom, lines)
        if (arms and UP != 0) canvas.drawRect(left, y, right, bottom, lines)
        if (arms and DOWN != 0) canvas.drawRect(left, top, right, y + cellHeight, lines)
    }

    /** Draws text at x, y, scaled down if it's wider than width (e.g. an emoji in one cell). */
    private fun drawFitted(canvas: Canvas, text: String, start: Int, end: Int, x: Float, y: Float, width: Float) {
        // ASCII fits its cell, other characters may not.
        val natural = if (end == start + 1 && text[start].code < 0x80) 0f else paint.measureText(text, start, end)

        if (natural <= width) {
            canvas.drawText(text, start, end, x, y + baseline, paint)
            return
        }

        // Scale it down and centre it vertically.
        val scale = width / natural
        paint.textSize = textSize * scale
        canvas.drawText(text, start, end, x, y + cellHeight / 2 - scale * glyphMiddle, paint)
        paint.textSize = textSize
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)

        if (!scaleDetector.isInProgress) {
            gestureDetector.onTouchEvent(event)
        }

        return true
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // Tell the keyboard this is a message field, so it suggests, corrects and swipes. Enter sends.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_SHORT_MESSAGE or
            InputType.TYPE_TEXT_FLAG_AUTO_CORRECT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_ENTER_ACTION or EditorInfo.IME_ACTION_NONE
        outAttrs.initialSelStart = keyboardLine.text.length
        outAttrs.initialSelEnd = keyboardLine.text.length
        return KeyboardConnection()
    }

    /** Lets the keyboard edit keyboardLine, which passes each change on to toxic. */
    private inner class KeyboardConnection : BaseInputConnection(this@TerminalView, true) {
        private var batchDepth = 0

        override fun getEditable() = keyboardLine.text

        override fun beginBatchEdit(): Boolean {
            ++batchDepth
            return true
        }

        override fun endBatchEdit(): Boolean {
            if (batchDepth > 0 && --batchDepth == 0) {
                reportSelection()
            }

            return batchDepth > 0
        }

        override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
            val string = text.toString()

            if (ctrlLatched && string.length == 1) {
                super.finishComposingText()
                type(string)
                return true
            }

            // Each line break sends the line so far, like Enter.
            for ((i, line) in string.split('\n').withIndex()) {
                if (i > 0) type("\r")
                super.commitText(line, newCursorPosition)
            }

            return true
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            // The keyboard can't see what toxic had before its line, so send toxic backspaces for that part.
            val known = minOf(beforeLength, Selection.getSelectionStart(keyboardLine.text))
            val done = super.deleteSurroundingText(known, afterLength)
            backspace(beforeLength - known)
            return done
        }

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
            val line = keyboardLine.text
            val known = minOf(beforeLength, Character.codePointCount(line, 0, Selection.getSelectionStart(line)))
            val done = super.deleteSurroundingTextInCodePoints(known, afterLength)
            backspace(beforeLength - known)
            return done
        }

        override fun performEditorAction(actionCode: Int): Boolean {
            type("\r")
            return true
        }

        private fun backspace(count: Int) = repeat(count) { Terminal.write(byteArrayOf(0x7f)) }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        keySequence(keyCode, event)?.let {
            type(it)
            return true
        }

        val ch = event.getUnicodeChar(event.metaState and (KeyEvent.META_CTRL_MASK or KeyEvent.META_ALT_MASK).inv())

        if (ch == 0 || ch and KeyCharacterMap.COMBINING_ACCENT != 0) {
            return super.onKeyDown(keyCode, event)
        }

        var text = String(Character.toChars(ch))

        if (event.isCtrlPressed) {
            text = withCtrl(text[0])
        }

        type(if (event.isAltPressed) "\u001b$text" else text)
        return true
    }

    private companion object {
        /** Cells with this background don't need drawing. */
        const val BACKGROUND = 0xff000000.toInt()
        const val SIZE_FG = 0xffd0d0d0.toInt()
        const val LEFT = 1
        const val RIGHT = 2
        const val UP = 4
        const val DOWN = 8
        const val MIN_TEXT_SP = 6f
        const val MAX_TEXT_SP = 40f
        const val DEFAULT_TEXT_SP = 13f
        const val SIZE_TEXT_SP = 20f
        const val SIZE_BG = 0xe0303030.toInt()
        const val SIZE_SHOWN_MS = 1000L
        const val PREFERENCES = "terminal"
        const val TEXT_SIZE = "text_size_sp"

        /** toxic's default keys for scrolling a line up and down. */
        const val SCROLL_UP = "\u0006"
        const val SCROLL_DOWN = "\u0016"

        fun withCtrl(c: Char): String {
            val upper = if (c in 'a'..'z') c.uppercaseChar() else c
            return if (upper in '@'..'_') (upper.code and 0x1f).toChar().toString() else upper.toString()
        }

        /**
         * Returns the lines (LEFT, RIGHT, UP, DOWN) from the middle of the cell for the box-drawing
         * characters ncurses draws boxes with, or 0 for any other character.
         */
        fun boxArms(ch: Char) = when (ch) {
            '─' -> LEFT or RIGHT
            '│' -> UP or DOWN
            '┌' -> RIGHT or DOWN
            '┐' -> LEFT or DOWN
            '└' -> RIGHT or UP
            '┘' -> LEFT or UP
            '├' -> UP or DOWN or RIGHT
            '┤' -> UP or DOWN or LEFT
            '┬' -> LEFT or RIGHT or DOWN
            '┴' -> LEFT or RIGHT or UP
            '┼' -> LEFT or RIGHT or UP or DOWN
            else -> 0
        }

        /** Returns what a key that isn't a character sends, as in toxic.terminfo, or null for other keys. */
        fun keySequence(keyCode: Int, event: KeyEvent) = when (keyCode) {
            KeyEvent.KEYCODE_DEL -> "\u007f"
            KeyEvent.KEYCODE_FORWARD_DEL -> "\u001b[3~"
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> "\r"
            KeyEvent.KEYCODE_TAB -> if (event.isShiftPressed) "\u001b[Z" else "\t"
            KeyEvent.KEYCODE_ESCAPE -> "\u001b"
            KeyEvent.KEYCODE_DPAD_UP -> "\u001b[A"
            KeyEvent.KEYCODE_DPAD_DOWN -> "\u001b[B"
            KeyEvent.KEYCODE_DPAD_RIGHT -> "\u001b[C"
            KeyEvent.KEYCODE_DPAD_LEFT -> "\u001b[D"
            KeyEvent.KEYCODE_MOVE_HOME -> "\u001b[H"
            KeyEvent.KEYCODE_MOVE_END -> "\u001b[F"
            KeyEvent.KEYCODE_INSERT -> "\u001b[2~"
            KeyEvent.KEYCODE_PAGE_UP -> "\u001b[5~"
            KeyEvent.KEYCODE_PAGE_DOWN -> "\u001b[6~"
            KeyEvent.KEYCODE_F1 -> "\u001bOP"
            KeyEvent.KEYCODE_F2 -> "\u001bOQ"
            KeyEvent.KEYCODE_F3 -> "\u001bOR"
            KeyEvent.KEYCODE_F4 -> "\u001bOS"
            KeyEvent.KEYCODE_F5 -> "\u001b[15~"
            KeyEvent.KEYCODE_F6 -> "\u001b[17~"
            KeyEvent.KEYCODE_F7 -> "\u001b[18~"
            KeyEvent.KEYCODE_F8 -> "\u001b[19~"
            KeyEvent.KEYCODE_F9 -> "\u001b[20~"
            KeyEvent.KEYCODE_F10 -> "\u001b[21~"
            KeyEvent.KEYCODE_F11 -> "\u001b[23~"
            KeyEvent.KEYCODE_F12 -> "\u001b[24~"
            else -> null
        }
    }
}
