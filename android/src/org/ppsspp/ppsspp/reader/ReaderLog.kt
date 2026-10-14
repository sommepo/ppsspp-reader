// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

/** The reader's lines in Android's log, under one tag. */
object ReaderLog {
    private const val TAG = "PPSSPPReader"

    fun info(message: String) = write(android.util.Log.INFO, message)

    fun warning(message: String) = write(android.util.Log.WARN, message)

    fun error(message: String) = write(android.util.Log.ERROR, message)

    private fun write(priority: Int, message: String) {
        try {
            android.util.Log.println(priority, TAG, message)
        } catch (_: RuntimeException) {
            // Off the device (plain JVM tests) there is no log to write to.
        }
    }
}
