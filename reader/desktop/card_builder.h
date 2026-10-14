// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <utility>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include "reader/desktop/dictionary.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp {

/// Everything a card can be made from. Only `line` is always present.
struct CardContext {
    TextLine line;
    QString game_title;
    /// The looked-up word, or nothing for a card of the sentence alone.
    std::optional<LookupResult> word;
    /// UTF-16 range of the word within the line's text: first and last index, or -1.
    int word_first{-1};
    int word_last{-1};
    /// Stored media file names, once the files are in Anki.
    QString screenshot_name;
    QString audio_name;
    /// The word as somebody says it (word_audio.h), once it is in Anki.
    QString word_audio_name;
    /// The game's page (SourceAddress::Of); empty when the game is not known.
    QString game_url;
};

/**
 * Fills note fields from templates such as "{expression}" or "{sentence}<br>{screenshot}".
 * A template may mix markers and ordinary text; an unknown marker is left as written so a
 * typo is visible on the card rather than silently empty.
 */
namespace CardBuilder {

/// Markers offered in the field mapping screen, with what they produce.
[[nodiscard]] QList<std::pair<QString, QString>> Markers();
/// Markers used anywhere in `templates`.
[[nodiscard]] QSet<QString> MarkersIn(const QStringList& templates);
[[nodiscard]] bool NeedsScreenshot(const QStringList& templates);
[[nodiscard]] bool NeedsAudio(const QStringList& templates);
[[nodiscard]] bool NeedsWordAudio(const QStringList& templates);
/// `mapping` with {word-audio} in the fields that are for the word's sound and have nothing in
/// them: a mapping made before that marker existed left them empty.
[[nodiscard]] QMap<QString, QString> OfferWordAudio(const QMap<QString, QString>& mapping);

/// Field values for `templates` (field name to template).
[[nodiscard]] QMap<QString, QString> Build(const QMap<QString, QString>& templates,
                                           const CardContext& context);

/// Parses a stored mapping: a JSON object of field name to template.
[[nodiscard]] QMap<QString, QString> ParseMapping(const QString& json);
[[nodiscard]] QString MappingToJson(const QMap<QString, QString>& mapping);
/**
 * A first guess at templates for a note type, from common field names used by popular
 * Japanese mining note types. Fields that match nothing stay empty for the user to fill in.
 */
[[nodiscard]] QMap<QString, QString> SuggestMapping(const QStringList& fields);

} // namespace CardBuilder

} // namespace ReaderApp
