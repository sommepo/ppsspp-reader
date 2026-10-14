// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QList>
#include <QString>

namespace ReaderApp::JapaneseText {

[[nodiscard]] bool IsHiragana(QChar c);
[[nodiscard]] bool IsKatakana(QChar c);
/// Kana of either script, including the long-vowel mark.
[[nodiscard]] bool IsKana(QChar c);
[[nodiscard]] bool IsKanji(char32_t code_point);
/// True for characters a dictionary term can contain: kana, kanji, letters and digits.
[[nodiscard]] bool IsTermChar(char32_t code_point);

[[nodiscard]] QString KatakanaToHiragana(const QString& text);
[[nodiscard]] QString HiraganaToKatakana(const QString& text);
/// Full-width ASCII letters and digits to their ordinary forms; other text unchanged.
[[nodiscard]] QString FullWidthToAscii(const QString& text);

/// A piece of a term: `text` with the `reading` to print above it, or none for kana.
struct FuriganaSegment {
    QString text;
    std::optional<QString> reading;

    bool operator==(const FuriganaSegment&) const = default;
};

/**
 * Splits `expression` into segments so that readings sit only over the kanji parts:
 * 食べ物 / たべもの gives 食(た) べ 物(もの). Kana inside the expression anchors the split. When
 * the kana cannot be matched against the reading, the whole reading goes over the whole
 * expression.
 */
[[nodiscard]] QList<FuriganaSegment> DistributeFurigana(const QString& expression,
                                                        const QString& reading);
/// DistributeFurigana in Anki's syntax: " 食[た]べ 物[もの]".
[[nodiscard]] QString AnkiFurigana(const QString& expression, const QString& reading);
/// DistributeFurigana as HTML ruby.
[[nodiscard]] QString RubyHtml(const QString& expression, const QString& reading);

} // namespace ReaderApp::JapaneseText
