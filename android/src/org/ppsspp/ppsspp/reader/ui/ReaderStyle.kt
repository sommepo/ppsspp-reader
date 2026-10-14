// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.ui

/** Colours of the reader's in-game surfaces. They sit over game pictures, so they are fixed
 * rather than following the app theme. */
object ReaderStyle {
    const val PANEL_BACKGROUND = 0xFF12161C.toInt() // alpha comes from the opacity setting
    const val POPUP_BACKGROUND = 0xF21A1F27.toInt()
    const val OUTLINE = 0x55FFFFFF
    const val TEXT = 0xFFF4F4F2.toInt()
    const val TEXT_DIM = 0xFFAEB4BC.toInt()
    const val TEXT_REREAD = 0xFFFF9E8A.toInt()
    const val ACCENT = 0xFFFFD54F.toInt()
    const val HIGHLIGHT = 0x66FFD54F

    /** The button boxes of the popup: a shade lighter than the popup, and opaque. */
    const val BOX = 0xFF2A313B.toInt()
    const val BOX_PRESSED = 0xFF55606E.toInt()

    /**
     * The word popup and the question about a card's audio are the console's own, as in the
     * desktop app ([ConsoleLook] draws their ground and the pointer).
     */
    const val POPUP_TEXT = 0xFFFFFFFF.toInt()
    const val POPUP_DIM = 0xFFD3E4F8.toInt()
    const val POPUP_ACCENT = 0xFFFFFFFF.toInt()
    const val POPUP_SIGN = 0xFFFFFFFF.toInt()
    const val POPUP_BOOK_SIGN = 0xFFFFFFFF.toInt()
    const val POPUP_BOX = 0xFF1E4696.toInt()
    const val POPUP_BOX_PRESSED = 0xFF3C78D2.toInt()
    const val POPUP_EDGE = 0x99FFFFFF.toInt()
    const val POPUP_BUTTON_TEXT = 0xFFFFFFFF.toInt()
    const val POPUP_BUTTON_KEEP = 0xFFFFFFFF.toInt()

    /** Over a part of the game's display while it is the cursor's trackpad. */
    const val TRACKPAD_DIM = 0x99000000.toInt()

    /**
     * The second display, which is black while it has nothing to do, as the cursor's trackpad:
     * a faint surface with an edge, so that it is plain where the finger goes.
     */
    const val PAD_SURFACE = 0xFF1B2028.toInt()

    /** What lies over the console's ground on the second display's trackpad, to quieten it. */
    const val PAD_VEIL = 0x66000000
    // The trackpad's edge and its word are in the console's own accent (POPUP_ACCENT).
    const val PAD_EDGE = 0x8CFFFFFF.toInt()
    const val PAD_HINT = 0x66FFFFFF
    const val PAD_HINT_ACTIVE = 0xCCFFFFFF.toInt()
    const val CURSOR = ACCENT
    const val CURSOR_EDGE = 0xD0000000.toInt()

    /** The session line in the corner: there when looked for, not when playing. */
    const val SESSION_TEXT = 0xB8FFFFFF.toInt()
    const val SESSION_SHADOW = 0xE6000000.toInt()
}
