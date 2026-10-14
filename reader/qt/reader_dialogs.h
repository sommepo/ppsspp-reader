// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QWidget;

namespace ReaderUi {

/// The reader's own windows, each run until it is closed. The game is paused by the caller.

/// The lines read so far, newest first: words in them can be looked up and cards made.
void RunBacklogDialog(QWidget* parent);
/// Reading statistics: today, the games, the reading days and the sessions.
void RunStatisticsDialog(QWidget* parent);
/// The installed dictionaries: importing, their order, switching off, deleting.
void RunDictionariesDialog(QWidget* parent);
/// Where cards go: the address of AnkiConnect, the deck, the note type and what each field
/// gets.
void RunAnkiSetupDialog(QWidget* parent);
/// The reader's settings.
void RunSettingsDialog(QWidget* parent);

} // namespace ReaderUi
