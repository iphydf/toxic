// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.text.Editable
import android.text.Selection
import android.text.Spanned
import android.text.TextWatcher

/**
 * Holds what the on-screen keyboard typed on toxic's input line since the last reset. The keyboard
 * edits it like a text field, so it can suggest, correct and swipe, and we send toxic every change.
 *
 * @param send gets what to type into toxic whenever text changes.
 */
class KeyboardLine(send: (String) -> Unit) {
    val text: Editable = Editable.Factory.getInstance().newEditable("")

    /** The line as toxic has it. */
    private var typed = ""

    init {
        text.setSpan(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence, start: Int, count: Int, after: Int) {}

            override fun onTextChanged(s: CharSequence, start: Int, before: Int, count: Int) {}

            override fun afterTextChanged(s: Editable) {
                val change = sync()

                if (change.isNotEmpty()) {
                    send(change)
                }
            }
        }, 0, 0, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
    }

    fun isEmpty() = text.isEmpty()

    /** Forgets the line. Call this when something other than the keyboard changed toxic's line. */
    fun reset() {
        typed = ""
        text.clear()
        Selection.setSelection(text, 0)
    }

    /**
     * Returns what makes toxic's line match text: a backspace for each character after the first
     * difference, then the rest of text. This assumes toxic's cursor is at the end of the line.
     */
    private fun sync(): String {
        val now = text.toString()
        // commonPrefixWith doesn't split a surrogate pair.
        val same = now.commonPrefixWith(typed).length
        val out = BACKSPACE.repeat(typed.codePointCount(same, typed.length)) + now.substring(same)
        typed = now
        return out
    }

    private companion object {
        const val BACKSPACE = "\u007f"
    }
}
