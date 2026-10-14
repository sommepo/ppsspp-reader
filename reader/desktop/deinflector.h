// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ReaderApp {

/// Word classes as dictionaries tag them in a term's "rules" field.
namespace WordClass {
inline constexpr int V1 = 1;     ///< ichidan verb
inline constexpr int V5 = 2;     ///< godan verb
inline constexpr int VS = 4;     ///< suru verb
inline constexpr int VK = 8;     ///< kuru verb
inline constexpr int VZ = 16;    ///< zuru verb
inline constexpr int ADJ_I = 32; ///< i-adjective
/// Not a dictionary class: the te-form, so that 食べている can pass through 食べて.
inline constexpr int TE = 64;
inline constexpr int ANY_VERB = V1 | V5 | VS | VK | VZ;

/// Parses a term's space-separated rules ("v1 vt", "v5", "adj-i", "vs"...).
[[nodiscard]] int Parse(const QString& rules);
} // namespace WordClass

/// A possible dictionary form of some text, with the inflections that were undone to reach it.
struct Deinflection {
    QString term;
    /// Word classes the dictionary entry must have (WordClass bits); 0 accepts any entry.
    int conditions{};
    /// Inflection names, outermost first: 食べさせられた gives [past, passive, causative].
    QStringList reasons;
};

/**
 * Undoes Japanese inflection by suffix rewriting: every rule replaces an inflected ending with
 * the ending of the form it was built from, and rules chain until nothing applies. The results
 * are candidates only; a candidate counts once a dictionary has that term with a matching word
 * class. The rules are those of the Android app's Deinflector, one for one.
 */
class Deinflector {
public:
    Deinflector();

    /// All candidates for `source`, the unchanged text first.
    [[nodiscard]] QList<Deinflection> Deinflect(const QString& source) const;

private:
    struct Rule {
        QString reason;
        QString suffix_in;
        QString suffix_out;
        int class_in;
        int class_out;
    };

    void Apply(const QList<int>& candidates, const Deinflection& current,
               QList<Deinflection>& results, QSet<QString>& seen) const;

    QList<Rule> rules;
    /// Rules by the last character of their inflected suffix; rules with no suffix apply to all.
    QHash<QChar, QList<int>> by_last_char;
    QList<int> unconditional;
};

} // namespace ReaderApp
