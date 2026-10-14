// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace ReaderApp {

/// A reading written above part of a line. Offsets count Unicode code points of TextLine::text.
struct RubySpan {
    int start{};
    int length{};
    QString reading;

    bool operator==(const RubySpan&) const = default;
};

/**
 * One piece of text the game presented, as reported by the text engine. The desktop sibling of
 * the Android app's TextLine: offsets into `text` are UTF-16, as there.
 */
struct TextLine {
    qint64 sequence{};
    qint64 frame{};
    /// Position in the emulated audio history when the line appeared, or -1.
    qint64 audio_position{};
    QString channel;
    QString speaker;
    QString text;
    QList<RubySpan> ruby;
    /// Stable identity of this text within the game: the same source again is rereading.
    QString source_id;
    QMap<QString, QString> meta;
    /// Wall-clock milliseconds when the line reached the app.
    qint64 timestamp{};
    /// What the game shows the text in, when its profile reports where text is on screen.
    QString window;

    [[nodiscard]] bool IsChoice() const;

    /// Text with readings in Anki's furigana syntax: " 漢字[かんじ]".
    [[nodiscard]] QString ToAnkiFurigana() const;
    /// Text as HTML with ruby elements. Line breaks become br elements.
    [[nodiscard]] QString ToRubyHtml() const;

    /// One run of text, with its reading when the run carries ruby.
    struct Segment {
        QString base;
        std::optional<QString> reading;
    };
    [[nodiscard]] QList<Segment> Segments() const;

    [[nodiscard]] QJsonObject ToJson() const;

    /// Parses the engine's event JSON. Nothing when the text is missing or empty.
    [[nodiscard]] static std::optional<TextLine> FromJson(const QByteArray& json,
                                                          qint64 timestamp);
    [[nodiscard]] static QJsonArray RubyToJson(const QList<RubySpan>& ruby);
    /// Parses ruby spans, dropping any that fall outside `text`.
    [[nodiscard]] static QList<RubySpan> RubyFromJson(const QJsonArray& array, const QString& text);
};

[[nodiscard]] QString ChannelDialogue();
[[nodiscard]] QString ChannelChoice();

[[nodiscard]] QString EscapeHtml(const QString& text);
/// Like Java's offsetByCodePoints, but clamps to the end of the string.
[[nodiscard]] int OffsetByCodePoints(const QString& text, int index, int code_points);
[[nodiscard]] int CodePointCount(const QString& text);
/// The code point that starts at `index` (a pair of surrogates counts as one).
[[nodiscard]] char32_t CodePointAt(const QString& text, int index);
/// How many UTF-16 units the code point at `index` takes: 1 or 2.
[[nodiscard]] int CharCountAt(const QString& text, int index);

} // namespace ReaderApp
