// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QJsonDocument>
#include <QJsonValue>
#include "reader/desktop/text_line.h"

namespace ReaderApp {

QString ChannelDialogue() {
    return QStringLiteral("dialogue");
}

QString ChannelChoice() {
    return QStringLiteral("choice");
}

QString EscapeHtml(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':
            out += QStringLiteral("&amp;");
            break;
        case u'<':
            out += QStringLiteral("&lt;");
            break;
        case u'>':
            out += QStringLiteral("&gt;");
            break;
        case u'"':
            out += QStringLiteral("&quot;");
            break;
        default:
            out += c;
        }
    }
    return out;
}

int CharCountAt(const QString& text, int index) {
    if (index < 0 || index >= text.size()) {
        return 1;
    }
    return text.at(index).isHighSurrogate() && index + 1 < text.size() &&
                   text.at(index + 1).isLowSurrogate()
               ? 2
               : 1;
}

char32_t CodePointAt(const QString& text, int index) {
    if (index < 0 || index >= text.size()) {
        return 0;
    }
    if (CharCountAt(text, index) == 2) {
        return QChar::surrogateToUcs4(text.at(index), text.at(index + 1));
    }
    return text.at(index).unicode();
}

int OffsetByCodePoints(const QString& text, int index, int code_points) {
    int i = std::clamp(index, 0, static_cast<int>(text.size()));
    int remaining = code_points;
    while (remaining > 0 && i < text.size()) {
        i += CharCountAt(text, i);
        --remaining;
    }
    return std::min(i, static_cast<int>(text.size()));
}

int CodePointCount(const QString& text) {
    int count = 0;
    for (int i = 0; i < text.size(); i += CharCountAt(text, i)) {
        ++count;
    }
    return count;
}

bool TextLine::IsChoice() const {
    return channel == ChannelChoice();
}

QList<TextLine::Segment> TextLine::Segments() const {
    QList<Segment> out;
    QList<RubySpan> sorted = ruby;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const RubySpan& a, const RubySpan& b) { return a.start < b.start; });
    int cursor = 0; // UTF-16 index
    int code_point = 0;
    for (const RubySpan& span : sorted) {
        if (span.start < code_point) {
            continue; // overlapping spans: keep the first
        }
        const int start = OffsetByCodePoints(text, cursor, span.start - code_point);
        const int end = OffsetByCodePoints(text, start, span.length);
        if (start > cursor) {
            out.append(Segment{text.mid(cursor, start - cursor), std::nullopt});
        }
        if (end > start) {
            out.append(Segment{text.mid(start, end - start), span.reading});
        }
        cursor = end;
        code_point = span.start + span.length;
    }
    if (cursor < text.size()) {
        out.append(Segment{text.mid(cursor), std::nullopt});
    }
    return out;
}

QString TextLine::ToAnkiFurigana() const {
    QString out;
    for (const Segment& segment : Segments()) {
        if (!segment.reading) {
            out += segment.base;
        } else {
            if (!out.isEmpty() && !out.back().isSpace()) {
                out += u' ';
            }
            out += segment.base + u'[' + *segment.reading + u']';
        }
    }
    return out;
}

QString TextLine::ToRubyHtml() const {
    QString out;
    for (const Segment& segment : Segments()) {
        QString escaped = EscapeHtml(segment.base);
        escaped.replace(u'\n', QStringLiteral("<br>"));
        if (!segment.reading) {
            out += escaped;
        } else {
            out += QStringLiteral("<ruby>") + escaped + QStringLiteral("<rt>") +
                   EscapeHtml(*segment.reading) + QStringLiteral("</rt></ruby>");
        }
    }
    return out;
}

QJsonArray TextLine::RubyToJson(const QList<RubySpan>& ruby) {
    QJsonArray out;
    for (const RubySpan& span : ruby) {
        out.append(QJsonArray{span.start, span.length, span.reading});
    }
    return out;
}

QList<RubySpan> TextLine::RubyFromJson(const QJsonArray& array, const QString& text) {
    QList<RubySpan> out;
    const int code_points = CodePointCount(text);
    for (const QJsonValue& value : array) {
        if (!value.isArray()) {
            continue;
        }
        const QJsonArray item = value.toArray();
        const int start = item.at(0).toInt(-1);
        const int length = item.at(1).toInt(0);
        const QString reading = item.at(2).toString();
        if (start < 0 || length <= 0 || reading.isEmpty()) {
            continue;
        }
        if (start + length > code_points) {
            continue;
        }
        out.append(RubySpan{start, length, reading});
    }
    return out;
}

QJsonObject TextLine::ToJson() const {
    QJsonObject out;
    out.insert(QStringLiteral("seq"), sequence);
    out.insert(QStringLiteral("frame"), frame);
    out.insert(QStringLiteral("audio"), audio_position);
    out.insert(QStringLiteral("channel"), channel);
    out.insert(QStringLiteral("speaker"), speaker);
    out.insert(QStringLiteral("text"), text);
    out.insert(QStringLiteral("ruby"), RubyToJson(ruby));
    out.insert(QStringLiteral("id"), source_id);
    QJsonObject meta_json;
    for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
        meta_json.insert(it.key(), it.value());
    }
    out.insert(QStringLiteral("meta"), meta_json);
    out.insert(QStringLiteral("time"), timestamp);
    if (!window.isEmpty()) {
        out.insert(QStringLiteral("window"), window);
    }
    return out;
}

namespace {

/// A JSON value as the text a card or a database row holds for it.
QString ValueText(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (number == static_cast<double>(static_cast<qint64>(number))) {
            return QString::number(static_cast<qint64>(number));
        }
        return QString::number(number);
    }
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QStringLiteral("null");
    case QJsonValue::Array:
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object:
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    return {};
}

} // namespace

std::optional<TextLine> TextLine::FromJson(const QByteArray& json, qint64 timestamp) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    const QString text = root.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        return std::nullopt;
    }
    TextLine line;
    line.sequence = root.value(QStringLiteral("seq")).toInteger();
    line.frame = root.value(QStringLiteral("frame")).toInteger();
    line.audio_position = root.value(QStringLiteral("audio")).toInteger();
    line.channel = root.value(QStringLiteral("channel")).toString();
    if (line.channel.isEmpty()) {
        line.channel = ChannelDialogue();
    }
    line.speaker = root.value(QStringLiteral("speaker")).toString();
    line.text = text;
    line.ruby = RubyFromJson(root.value(QStringLiteral("ruby")).toArray(), text);
    line.source_id = root.value(QStringLiteral("id")).toString();
    const QJsonObject meta_json = root.value(QStringLiteral("meta")).toObject();
    for (auto it = meta_json.constBegin(); it != meta_json.constEnd(); ++it) {
        line.meta.insert(it.key(), ValueText(it.value()));
    }
    line.timestamp = root.contains(QStringLiteral("time"))
                         ? root.value(QStringLiteral("time")).toInteger(timestamp)
                         : timestamp;
    line.window = root.value(QStringLiteral("window")).toString();
    return line;
}

} // namespace ReaderApp
