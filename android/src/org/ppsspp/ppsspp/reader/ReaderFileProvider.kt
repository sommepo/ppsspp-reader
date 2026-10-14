// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader

import androidx.core.content.FileProvider

/**
 * Hands the reader's files to other apps: card media for AnkiDroid and statistics exports. A
 * class of its own because the emulator already declares a FileProvider, and a manifest can
 * name a class only once.
 */
class ReaderFileProvider : FileProvider()
