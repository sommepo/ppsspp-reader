// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace ReaderApp::Glossary {

/**
 * Turns a dictionary entry's glossary (plain strings or Yomitan "structured content") into
 * HTML. Full keeps the structure (lists, tables, ruby, inline styles and the data attributes
 * dictionary stylesheets rely on) and is what goes into Anki cards; Simple uses only a few
 * inline tags. Images are left out in both.
 */
enum class Mode { Full, Simple };

/// The definitions of one glossary, each as an HTML fragment.
[[nodiscard]] QStringList Definitions(const QString& glossary_json, Mode mode);
/// One glossary as a single HTML fragment: a list when it has several definitions.
[[nodiscard]] QString ToHtml(const QString& glossary_json, Mode mode);
/// The glossary as plain text, definitions separated by "; ".
[[nodiscard]] QString ToPlainText(const QString& glossary_json);
[[nodiscard]] QString StripTags(const QString& html);

/// A definition cut down to one line: its glosses, and remarks about how the word is used.
struct Compact {
    QString glosses;
    QString notes;
};

/**
 * The definitions of one glossary as short plain text, for a popup that is to stay small.
 * Where the dictionary marks what is what (JMdict does), only the glosses and the notes are
 * kept: cross-references, example sentences and tables of forms are left out. A dictionary
 * that marks nothing is kept whole, its blocks run together.
 */
[[nodiscard]] QList<Compact> CompactDefinitions(const QString& glossary_json);

} // namespace ReaderApp::Glossary
