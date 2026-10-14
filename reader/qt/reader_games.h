// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QList>
#include <QPixmap>
#include <QString>
#include "reader/core/types.h"
#include "reader/desktop/catalog.h"

class QWidget;

namespace ReaderUi {

/**
 * The games whose text can be read: those with a profile that comes with the app, and those
 * the user has put a profile of their own in the app's folder for.
 */
namespace Games {
/// Every game with a profile, by title. The user's own profile of a game stands for it.
[[nodiscard]] QList<ReaderApp::ProfileInfo> Supported();
/// True when the game `program_id` has a profile.
[[nodiscard]] bool Covers(const QString& game_id);
/// What the profile of the game `title_id` says of its lines' audio ("never", "ask",
/// "always"), or an empty string.
[[nodiscard]] QString AudioOf(const QString& title_id);
/// Looks again at the next question: after a profile was added or removed.
void Forget();
/**
 * The mark of a game whose text the reader can read: `picture`, a game's picture in the game
 * list, with a small boxed book in its top right corner when the game `program_id` has a
 * text profile; unchanged otherwise.
 */
[[nodiscard]] QPixmap Badged(const QPixmap& picture, const QString& game_id);
} // namespace Games

/**
 * The game scripts: those installed and those of the published list, each game's name, what
 * is read, the version the script was written for, its ID and how it stands; installing,
 * updating and removing one, and installing files of the user's own.
 */
void RunGameScriptsDialog(QWidget* parent);

} // namespace ReaderUi
