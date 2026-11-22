// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/** Reads and writes toxic's config file with the same libconfig toxic uses (config.c). */
object ToxicConfig {
    init {
        System.loadLibrary("toxic")
    }

    // libconfig's setting types (CONFIG_TYPE_*). REMOVE is CONFIG_TYPE_NONE.
    const val REMOVE = 0
    const val GROUP = 1
    const val INT = 2
    const val INT64 = 3
    const val FLOAT = 4
    const val STRING = 5
    const val BOOL = 6

    /** A group or value in a config file. line counts from 1. */
    class Entry(val path: String, val type: Int, val line: Int, val value: String)

    /** Sets path to value, or removes it if type is REMOVE. */
    class Change(val path: String, val type: Int, val value: String)

    @JvmStatic
    private external fun parse(text: ByteArray): ByteArray

    @JvmStatic
    private external fun update(text: ByteArray, changes: ByteArray): ByteArray

    /** toxic reads $XDG_CONFIG_HOME/tox/toxic.conf, and session.c sets XDG_CONFIG_HOME to files/.config. */
    fun file(context: Context) = File(context.filesDir, ".config/tox/toxic.conf")

    /** Returns every group and value in text. Throws IllegalArgumentException("line N: ...") on a syntax error. */
    fun entries(text: String) = fields(parse(text.toByteArray())).chunked(4).filter { it.size == 4 }
        .map { (path, type, line, value) -> Entry(path, type.toInt(), line.toInt(), value) }

    /**
     * Returns text with changes made. Throws IllegalArgumentException on a syntax error or a value of
     * the wrong type.
     */
    fun update(text: String, changes: List<Change>): String {
        val fields = changes.joinToString("") { "${it.path}\u0000${it.type}\u0000${it.value}\u0000" }
        return String(update(text.toByteArray(), fields.toByteArray()))
    }

    /** Returns "" if the file doesn't exist. */
    fun read(file: File) = if (file.exists()) file.readText() else ""

    /** Replaces a file with text, so toxic never reads half of it. */
    fun write(file: File, text: String) {
        val parent = file.parentFile

        if (parent != null && !parent.isDirectory && !parent.mkdirs()) {
            throw IOException("can't make $parent")
        }

        val temp = File(file.path + ".new")

        FileOutputStream(temp).use { out ->
            out.write(text.toByteArray())
            out.fd.sync()
        }

        if (!temp.renameTo(file)) {
            throw IOException("can't replace $file")
        }
    }

    private fun fields(bytes: ByteArray) = String(bytes).split('\u0000')
}
