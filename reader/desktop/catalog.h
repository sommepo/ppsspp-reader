// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QString>

namespace ReaderApp {

/// What a text profile says of itself in its first lines.
struct ProfileInfo {
    QString title_id;
    /// The game's name; the title ID when the profile does not say.
    QString title;
    /// What of the game's text is read, in a line; may be empty.
    QString reads;
    /// The version of the game the profile was written for; may be empty.
    QString version;
    /// A profile the user put in the app's folder, which is used instead of a bundled one.
    bool own{};
    /// What the profile says a card does with its line's audio ("never", "ask" or "always"),
    /// for a game whose lines have no voices; empty when it says nothing. The user's own
    /// choice for the game goes before it.
    QString audio;
};

/// Which games the reader can read, from the profiles themselves.
namespace Catalog {
/**
 * Reads the lines "-- reader: <key> <value>" among the comments a profile begins with: title,
 * reads, version and audio. Other lines, and other keys, are passed over.
 */
[[nodiscard]] ProfileInfo Describe(const QString& title_id, const QString& source, bool own);
/// The title ID a profile file of this name is for, or nothing when it is not a profile.
[[nodiscard]] std::optional<QString> TitleIdOf(const QString& file_name);
} // namespace Catalog

} // namespace ReaderApp
