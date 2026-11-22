// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

/**
 * Reads the settings screen from toxic's example config, which has every setting with its default
 * value. A setting's description is the comment above it.
 */
object ConfigSchema {
    /**
     * Not shown: sounds (the app doesn't have toxic's sound files yet), friends, groupchats and
     * conferences (settings per public key) and blocked_words (a list).
     */
    private val SECTIONS = listOf("ui", "audio", "tox", "keys")

    /** A setting the example only shows in a comment, like `// download_path="/home/...";`. */
    private val COMMENTED = Regex("""^\s*//\s*(\w+)\s*=\s*"(.*)"\s*;?\s*$""")

    /** A comment line listing the values a setting can have, like "colors: black, white, gray". */
    private val CHOICES = Regex("""^\w+: (\w+(?:, \w+)+)$""")

    class Section(val name: String, val description: String) {
        val options = mutableListOf<Option>()
    }

    class Option(
        /** E.g. "ui.timestamps". */
        val path: String,
        val name: String,
        val type: Int,
        val defaultValue: String,
        val description: String,
        /** Empty if any value of its type is allowed. */
        val choices: List<String>,
        /** An example value for a setting that has none by default. */
        val hint: String?,
        val line: Int,
    )

    fun read(example: String): List<Section> {
        val lines = example.split('\n')
        val sections = LinkedHashMap<String, Section>()
        val topLevel = mutableListOf<ToxicConfig.Entry>()

        for (entry in ToxicConfig.entries(example)) {
            val parts = entry.path.split('.')

            if (parts.size == 1 && entry.type == ToxicConfig.GROUP) {
                topLevel += entry

                if (entry.path in SECTIONS) {
                    sections[entry.path] = Section(entry.path, comment(lines, entry.line).joinToString(" "))
                }
            } else if (parts.size == 2 && entry.type != ToxicConfig.GROUP) {
                sections[parts[0]]?.options?.add(
                    option(lines, entry.path, parts[1], entry.type, entry.value, null, entry.line),
                )
            }
        }

        lines.forEachIndexed { i, text ->
            val setting = COMMENTED.matchEntire(text) ?: return@forEachIndexed
            val line = i + 1
            // The line belongs to the last top-level group that starts before it.
            val section = topLevel.lastOrNull { it.line < line }?.path?.let(sections::get) ?: return@forEachIndexed
            val name = setting.groupValues[1]
            section.options +=
                option(lines, "${section.name}.$name", name, ToxicConfig.STRING, "", setting.groupValues[2], line)
        }

        for (section in sections.values) {
            section.options.sortBy { it.line }
        }

        return sections.values.toList()
    }

    private fun option(
        lines: List<String>,
        path: String,
        name: String,
        type: Int,
        defaultValue: String,
        hint: String?,
        line: Int,
    ): Option {
        val (choices, description) = comment(lines, line).partition(CHOICES::matches)
        return Option(
            path,
            name,
            type,
            defaultValue,
            description.joinToString(" "),
            choices.lastOrNull()?.let { CHOICES.matchEntire(it)!!.groupValues[1].split(", ") } ?: emptyList(),
            hint,
            line,
        )
    }

    /** Returns the comment lines right above line (counted from 1), without the "//". */
    private fun comment(lines: List<String>, line: Int) = lines.subList(0, line - 1).asReversed()
        .map(String::trim)
        .takeWhile { it.startsWith("//") && !COMMENTED.matches(it) }
        .map { it.substring(2).trim() }
        .asReversed()
}
