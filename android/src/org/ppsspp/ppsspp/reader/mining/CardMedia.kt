// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.mining

import java.io.File

/**
 * Where a card's picture and audio come from: the running game ([MediaCapture]), or a stand-in
 * where there is no emulator. Callbacks run on the main thread.
 */
interface CardMedia {
    /** A screenshot of the current frame in a new file, or null. */
    fun captureNow(callback: (File?) -> Unit)

    /** The kept screenshot of a backlog line, or null. */
    fun screenshotFor(backlogId: Long): File?

    /**
     * What the game played during line [backlogId], between two positions of the audio history
     * ([end] null: until now), as a clip in a new file. Null when nothing audible is there.
     */
    fun audioClip(backlogId: Long, start: Long, end: Long?, callback: (File?) -> Unit)
}
