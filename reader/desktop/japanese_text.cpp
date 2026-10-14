// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>
#include "reader/desktop/japanese_text.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp::JapaneseText {

bool IsHiragana(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'ぁ' && u <= u'ゖ') || u == u'ゝ' || u == u'ゞ';
}

bool IsKatakana(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'ァ' && u <= u'ヺ') || u == u'ヽ' || u == u'ヾ';
}

bool IsKana(QChar c) {
    return IsHiragana(c) || IsKatakana(c) || c.unicode() == u'ー';
}

bool IsKanji(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F) || cp == 0x3005 ||
           cp == 0x3007;
}

bool IsTermChar(char32_t cp) {
    if (IsKanji(cp)) {
        return true;
    }
    if (cp <= 0xFFFF && IsKana(QChar(static_cast<char16_t>(cp)))) {
        return true;
    }
    // Letters and decimal digits, as Java's Character.isLetterOrDigit has it.
    return QChar::isLetter(cp) || QChar::category(cp) == QChar::Number_DecimalDigit;
}

QString KatakanaToHiragana(const QString& text) {
    QString out = text;
    for (QChar& c : out) {
        const char16_t u = c.unicode();
        if ((u >= u'ァ' && u <= u'ヶ') || u == u'ヽ' || u == u'ヾ') {
            c = QChar(static_cast<char16_t>(u - 0x60));
        }
    }
    return out;
}

QString HiraganaToKatakana(const QString& text) {
    QString out = text;
    for (QChar& c : out) {
        const char16_t u = c.unicode();
        if ((u >= u'ぁ' && u <= u'ゖ') || u == u'ゝ' || u == u'ゞ') {
            c = QChar(static_cast<char16_t>(u + 0x60));
        }
    }
    return out;
}

QString FullWidthToAscii(const QString& text) {
    QString out = text;
    for (QChar& c : out) {
        const char16_t u = c.unicode();
        if (u >= u'！' && u <= u'～') {
            c = QChar(static_cast<char16_t>(u - 0xFEE0));
        }
    }
    return out;
}

namespace {

using Group = std::pair<QString, bool>; // text, is kana

std::optional<QList<FuriganaSegment>> Match(const QList<Group>& groups, int index,
                                            const QString& normalized, const QString& original,
                                            int position) {
    if (index == groups.size()) {
        if (position == normalized.size()) {
            return QList<FuriganaSegment>{};
        }
        return std::nullopt;
    }
    const auto& [text, kana] = groups.at(index);
    if (kana) {
        const QString plain = KatakanaToHiragana(text);
        if (QStringView(normalized).mid(position, plain.size()) != plain) {
            return std::nullopt;
        }
        auto rest = Match(groups, index + 1, normalized, original,
                          position + static_cast<int>(plain.size()));
        if (!rest) {
            return std::nullopt;
        }
        rest->prepend(FuriganaSegment{text, std::nullopt});
        return rest;
    }
    // A non-kana group takes at least one reading character; try the shortest split first.
    const bool last = index == groups.size() - 1;
    const int from = position + 1;
    const int to = static_cast<int>(normalized.size());
    for (int end = last ? to : from; end <= to; ++end) {
        auto rest = Match(groups, index + 1, normalized, original, end);
        if (rest) {
            rest->prepend(FuriganaSegment{text, original.mid(position, end - position)});
            return rest;
        }
    }
    return std::nullopt;
}

} // namespace

QList<FuriganaSegment> DistributeFurigana(const QString& expression, const QString& reading) {
    if (reading.isEmpty() || reading == expression) {
        return {FuriganaSegment{expression, std::nullopt}};
    }
    const QString normalized = KatakanaToHiragana(reading);
    // Runs of kana and runs of everything else.
    QList<Group> groups;
    int start = 0;
    bool all_kana = true;
    while (start < expression.size()) {
        const bool kana = IsKana(expression.at(start));
        int end = start + 1;
        while (end < expression.size() && IsKana(expression.at(end)) == kana) {
            ++end;
        }
        groups.append({expression.mid(start, end - start), kana});
        all_kana = all_kana && kana;
        start = end;
    }
    if (all_kana) {
        return {FuriganaSegment{expression, std::nullopt}};
    }
    if (auto result = Match(groups, 0, normalized, reading, 0)) {
        return *result;
    }
    return {FuriganaSegment{expression, reading}};
}

QString AnkiFurigana(const QString& expression, const QString& reading) {
    QString out;
    for (const FuriganaSegment& segment : DistributeFurigana(expression, reading)) {
        if (!segment.reading) {
            out += segment.text;
        } else {
            if (!out.isEmpty()) {
                out += u' ';
            }
            out += segment.text + u'[' + *segment.reading + u']';
        }
    }
    return out;
}

QString RubyHtml(const QString& expression, const QString& reading) {
    QString out;
    for (const FuriganaSegment& segment : DistributeFurigana(expression, reading)) {
        if (!segment.reading) {
            out += EscapeHtml(segment.text);
        } else {
            out += QStringLiteral("<ruby>") + EscapeHtml(segment.text) + QStringLiteral("<rt>") +
                   EscapeHtml(*segment.reading) + QStringLiteral("</rt></ruby>");
        }
    }
    return out;
}

} // namespace ReaderApp::JapaneseText
