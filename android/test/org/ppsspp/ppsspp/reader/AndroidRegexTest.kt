// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * These tests run on a desktop JVM, whose regular expressions are more forgiving than
 * Android's: a pattern like `\{([a-z-]+)}` works here and throws on a device, where a brace
 * that is not part of a quantifier has to be escaped. That took the app down the first time a
 * card was built on a real device. So the reader's patterns are checked from their source.
 */
class AndroidRegexTest {
    /** What is wrong with [pattern] for Android's engine, or null. */
    private fun problem(pattern: String): String? {
        var i = 0
        var inClass = false
        while (i < pattern.length) {
            val c = pattern[i]
            when {
                c == '\\' -> {
                    // \Q...\E quotes everything in between.
                    if (pattern.startsWith("\\Q", i)) {
                        val end = pattern.indexOf("\\E", i + 2)
                        i = if (end < 0) pattern.length else end + 1
                    } else {
                        i++
                    }
                }

                inClass -> if (c == ']') inClass = false else if (c == '{' || c == '}') return "bare $c in a class"

                c == '[' -> inClass = true

                c == '{' -> {
                    val quantifier = Regex("^\\{\\d+(,\\d*)?\\}").find(pattern.substring(i))
                        ?: return "bare { at $i"
                    i += quantifier.value.length - 1
                }

                c == '}' -> return "bare } at $i"
            }
            i++
        }
        return null
    }

    @Test
    fun theCheckKnowsWhatAndroidRejects() {
        assertEquals("bare } at 11", problem("\\{([a-z-]+)}"))
        assertEquals("bare { at 0", problem("{name}"))
        assertEquals("bare } in a class", problem("[a}]"))
        assertNull(problem("\\{([a-z\\-]+)\\}"))
        assertNull(problem("\n{2,}"))
        assertNull(problem("a{3}b{1,4}"))
        assertNull(problem("\\Q{x}\\E_(\\d+)\\.json"))
    }

    @Test
    fun theReadersPatternsAreAcceptedByAndroid() {
        val root = File("src/org/ppsspp/ppsspp/reader")
        assertTrue("run from the app module: ${root.absolutePath}", root.isDirectory)
        // A Regex("...") with a plain string literal; \\ and \" inside it are Kotlin escapes.
        val literal = Regex("Regex\\(\"((?:[^\"\\\\]|\\\\.)*)\"")
        val problems = ArrayList<String>()
        var seen = 0
        root.walkTopDown().filter { it.extension == "kt" }.forEach { file ->
            file.readLines().forEachIndexed { number, line ->
                for (match in literal.findAll(line)) {
                    seen++
                    val pattern = match.groupValues[1]
                        .replace("\\\\", "\u0000").replace("\\n", "\n").replace("\\\"", "\"")
                        .replace("\u0000", "\\")
                    problem(pattern)?.let { problems.add("${file.name}:${number + 1}: $pattern: $it") }
                }
            }
        }
        assertTrue("found the patterns to check ($seen)", seen >= 8)
        assertTrue(problems.joinToString("\n"), problems.isEmpty())
    }
}
