// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initializer_list>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include "reader/desktop/card_builder.h"
#include "reader/desktop/glossary.h"
#include "reader/desktop/japanese_text.h"

namespace ReaderApp::CardBuilder {

namespace {

const QRegularExpression& MarkerPattern() {
    static const QRegularExpression pattern(QStringLiteral("\\{([a-z\\-]+)\\}"));
    return pattern;
}

QString Breaks(QString text) {
    text.replace(u'\n', QStringLiteral("<br>"));
    return text;
}

/// The line as HTML with [first, last] in bold.
QString SentenceHtml(const QString& text, int first, int last) {
    if (first < 0 || last < first || last >= text.size()) {
        return Breaks(EscapeHtml(text));
    }
    return Breaks(EscapeHtml(text.left(first)) + QStringLiteral("<b>") +
                  EscapeHtml(text.mid(first, last - first + 1)) + QStringLiteral("</b>") +
                  EscapeHtml(text.mid(last + 1)));
}

QStringList Tags(const QString& tags) {
    return tags.split(u' ', Qt::SkipEmptyParts);
}

/**
 * Definitions grouped by dictionary, in the list structure Yomitan's cards use, so existing
 * card styling for ".yomitan-glossary" keeps working.
 */
QString GlossaryHtml(const LookupResult& word, bool first_only) {
    QList<TermEntry> entries = word.entries;
    if (first_only && !entries.isEmpty()) {
        const QString first = entries.first().dictionary;
        entries.removeIf([&first](const TermEntry& entry) { return entry.dictionary != first; });
    }
    if (entries.isEmpty()) {
        return {};
    }
    QString out = QStringLiteral("<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>");
    for (const TermEntry& entry : entries) {
        const QString body = Glossary::ToHtml(entry.glossary_json, Glossary::Mode::Full);
        if (body.isEmpty()) {
            continue;
        }
        out += QStringLiteral("<li data-dictionary=\"") + EscapeHtml(entry.dictionary) +
               QStringLiteral("\"><i>(");
        const QStringList tags = Tags(entry.definition_tags);
        if (!tags.isEmpty()) {
            out += EscapeHtml(tags.join(QStringLiteral(", "))) + QStringLiteral(", ");
        }
        out += EscapeHtml(entry.dictionary) + QStringLiteral(")</i> ") + body +
               QStringLiteral("</li>");
    }
    out += QStringLiteral("</ol></div>");
    return out;
}

/// What `marker` stands for, or nothing for a marker that is not known.
std::optional<QString> Value(const QString& marker, const CardContext& c) {
    const std::optional<LookupResult>& word = c.word;
    const auto is = [&marker](const char16_t* name) { return marker == QString::fromUtf16(name); };
    if (is(u"expression")) {
        // A card of the sentence alone has no word. The line stands in for it, so that a note
        // type made for words (whose first field is the word) still gets a note.
        return word ? word->expression : Breaks(EscapeHtml(c.line.text));
    }
    if (is(u"reading")) {
        return word ? (word->reading.isEmpty() ? word->expression : word->reading) : QString();
    }
    if (is(u"furigana")) {
        return word ? JapaneseText::AnkiFurigana(word->expression, word->reading)
                    : Breaks(EscapeHtml(c.line.ToAnkiFurigana()));
    }
    if (is(u"furigana-html")) {
        return word ? JapaneseText::RubyHtml(word->expression, word->reading)
                    : c.line.ToRubyHtml();
    }
    if (is(u"glossary")) {
        return word ? GlossaryHtml(*word, false) : QString();
    }
    if (is(u"glossary-first")) {
        return word ? GlossaryHtml(*word, true) : QString();
    }
    if (is(u"glossary-plain")) {
        if (!word) {
            return QString();
        }
        QStringList parts;
        for (const TermEntry& entry : word->entries) {
            parts.append(Glossary::ToPlainText(entry.glossary_json));
        }
        return Breaks(EscapeHtml(parts.join(u'\n')));
    }
    if (is(u"sentence")) {
        return SentenceHtml(c.line.text, c.word_first, c.word_last);
    }
    if (is(u"sentence-plain")) {
        return Breaks(EscapeHtml(c.line.text));
    }
    if (is(u"sentence-furigana")) {
        return Breaks(EscapeHtml(c.line.ToAnkiFurigana()));
    }
    if (is(u"sentence-furigana-html")) {
        return c.line.ToRubyHtml();
    }
    if (is(u"screenshot")) {
        return c.screenshot_name.isEmpty()
                   ? QString()
                   : QStringLiteral("<img src=\"") + EscapeHtml(c.screenshot_name) +
                         QStringLiteral("\">");
    }
    if (is(u"audio")) {
        return c.audio_name.isEmpty() ? QString()
                                      : QStringLiteral("[sound:") + c.audio_name + u']';
    }
    if (is(u"word-audio")) {
        return c.word_audio_name.isEmpty() ? QString()
                                           : QStringLiteral("[sound:") + c.word_audio_name + u']';
    }
    if (is(u"speaker")) {
        return EscapeHtml(c.line.speaker);
    }
    if (is(u"source")) {
        return EscapeHtml(c.game_title);
    }
    if (is(u"url")) {
        return EscapeHtml(c.game_url);
    }
    if (is(u"frequencies")) {
        if (!word) {
            return QString();
        }
        QStringList parts;
        for (const Frequency& frequency : word->frequencies) {
            parts.append(EscapeHtml(frequency.dictionary + QStringLiteral(": ") + frequency.display));
        }
        return parts.join(QStringLiteral("<br>"));
    }
    if (is(u"frequency-rank")) {
        if (!word) {
            return QString();
        }
        const std::optional<qint64> rank = DictionaryLookup::HarmonicRank(word->frequencies);
        return rank ? QString::number(*rank) : QString();
    }
    if (is(u"pitch-positions")) {
        if (!word) {
            return QString();
        }
        QStringList positions;
        for (const PitchAccent& pitch : word->pitches) {
            for (const int position : pitch.positions) {
                const QString text = QString::number(position);
                if (!positions.contains(text)) {
                    positions.append(text);
                }
            }
        }
        return positions.join(QStringLiteral(", "));
    }
    if (is(u"part-of-speech")) {
        if (!word) {
            return QString();
        }
        QStringList tags;
        for (const TermEntry& entry : word->entries) {
            for (const QString& tag : Tags(entry.definition_tags)) {
                if (!tags.contains(tag)) {
                    tags.append(tag);
                }
            }
        }
        return EscapeHtml(tags.join(QStringLiteral(", ")));
    }
    if (is(u"inflection")) {
        return word ? EscapeHtml(word->reasons.join(QStringLiteral(" « "))) : QString();
    }
    if (is(u"matched-text")) {
        return word ? EscapeHtml(word->matched_text) : QString();
    }
    return std::nullopt;
}

} // namespace

QList<std::pair<QString, QString>> Markers() {
    return {
        {QStringLiteral("expression"),
         QStringLiteral("Word in dictionary form (the line, on a sentence card)")},
        {QStringLiteral("reading"), QStringLiteral("Reading in kana")},
        {QStringLiteral("furigana"), QStringLiteral("Word with reading: 食[た]べる")},
        {QStringLiteral("furigana-html"), QStringLiteral("Word with reading as ruby HTML")},
        {QStringLiteral("glossary"), QStringLiteral("Definitions from every dictionary")},
        {QStringLiteral("glossary-first"), QStringLiteral("Definitions from the first dictionary")},
        {QStringLiteral("glossary-plain"), QStringLiteral("Definitions as plain text")},
        {QStringLiteral("sentence"), QStringLiteral("The line, with the word in bold")},
        {QStringLiteral("sentence-plain"), QStringLiteral("The line as plain text")},
        {QStringLiteral("sentence-furigana"),
         QStringLiteral("The line with the game's furigana: 君[きみ]")},
        {QStringLiteral("sentence-furigana-html"),
         QStringLiteral("The line with the game's furigana as ruby HTML")},
        {QStringLiteral("screenshot"), QStringLiteral("Picture of the game screen")},
        {QStringLiteral("audio"), QStringLiteral("Game audio of the line")},
        {QStringLiteral("word-audio"), QStringLiteral("The word spoken (JapanesePod101)")},
        {QStringLiteral("speaker"), QStringLiteral("Who is speaking")},
        {QStringLiteral("source"), QStringLiteral("Game title")},
        {QStringLiteral("url"), QStringLiteral("The game's page, as a web address")},
        {QStringLiteral("frequencies"), QStringLiteral("Frequency list entries")},
        {QStringLiteral("frequency-rank"), QStringLiteral("One frequency rank for sorting")},
        {QStringLiteral("pitch-positions"), QStringLiteral("Pitch accent positions")},
        {QStringLiteral("part-of-speech"), QStringLiteral("Word class tags")},
        {QStringLiteral("inflection"), QStringLiteral("How the word was inflected in the line")},
        {QStringLiteral("matched-text"), QStringLiteral("The word as written in the line")},
    };
}

QSet<QString> MarkersIn(const QStringList& templates) {
    QSet<QString> out;
    for (const QString& text : templates) {
        auto matches = MarkerPattern().globalMatch(text);
        while (matches.hasNext()) {
            out.insert(matches.next().captured(1));
        }
    }
    return out;
}

bool NeedsScreenshot(const QStringList& templates) {
    return MarkersIn(templates).contains(QStringLiteral("screenshot"));
}

bool NeedsAudio(const QStringList& templates) {
    return MarkersIn(templates).contains(QStringLiteral("audio"));
}

bool NeedsWordAudio(const QStringList& templates) {
    return MarkersIn(templates).contains(QStringLiteral("word-audio"));
}

namespace {
/// Fields that are for the word's own sound, by the names popular note types give them.
bool IsWordAudioField(const QString& field) {
    static const QRegularExpression separators{QStringLiteral("[\\s_\\-]")};
    QString name = field.toLower();
    name.remove(separators);
    static const QStringList names{QStringLiteral("wordaudio"),  QStringLiteral("expressionaudio"),
                                   QStringLiteral("vocabaudio"), QStringLiteral("termaudio"),
                                   QStringLiteral("audio"),      QStringLiteral("vocabularyaudio")};
    return names.contains(name);
}
} // namespace

QMap<QString, QString> OfferWordAudio(const QMap<QString, QString>& mapping) {
    QMap<QString, QString> out = mapping;
    for (auto it = out.begin(); it != out.end(); ++it) {
        if (it.value().trimmed().isEmpty() && IsWordAudioField(it.key())) {
            it.value() = QStringLiteral("{word-audio}");
        }
    }
    return out;
}

QMap<QString, QString> Build(const QMap<QString, QString>& templates, const CardContext& context) {
    QMap<QString, QString> out;
    for (auto it = templates.constBegin(); it != templates.constEnd(); ++it) {
        const QString& text = it.value();
        QString value;
        qsizetype position = 0;
        auto matches = MarkerPattern().globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            value += text.mid(position, match.capturedStart() - position);
            const std::optional<QString> replacement = Value(match.captured(1), context);
            value += replacement ? *replacement : match.captured(0);
            position = match.capturedEnd();
        }
        value += text.mid(position);
        out.insert(it.key(), value);
    }
    return out;
}

QMap<QString, QString> ParseMapping(const QString& json) {
    QMap<QString, QString> out;
    if (json.trimmed().isEmpty()) {
        return out;
    }
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject()) {
        return out;
    }
    const QJsonObject root = document.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        out.insert(it.key(), it.value().toString());
    }
    return out;
}

QString MappingToJson(const QMap<QString, QString>& mapping) {
    QJsonObject root;
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        root.insert(it.key(), it.value());
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QMap<QString, QString> SuggestMapping(const QStringList& fields) {
    static const QRegularExpression separators(QStringLiteral("[\\s_\\-]"));
    const auto among = [](const QString& name, std::initializer_list<const char*> names) {
        for (const char* candidate : names) {
            if (name == QLatin1String(candidate)) {
                return true;
            }
        }
        return false;
    };
    QMap<QString, QString> out;
    for (const QString& field : fields) {
        QString name = field.toLower();
        name.remove(separators);
        QString marker;
        if (among(name, {"expressionfurigana", "furigana", "vocabfurigana", "wordfurigana"})) {
            marker = QStringLiteral("{furigana}");
        } else if (among(name, {"expressionreading", "reading", "vocabreading", "wordreading",
                                "kana", "vocabkana"})) {
            marker = QStringLiteral("{reading}");
        } else if (among(name, {"expression", "word", "vocab", "vocabulary", "vocabkanji", "key",
                                "target", "term", "front"})) {
            marker = QStringLiteral("{expression}");
        } else if (among(name, {"sentencefurigana", "sentencereading"})) {
            marker = QStringLiteral("{sentence-furigana}");
        } else if (among(name, {"sentence", "context", "example", "sentencekanji",
                                "expressionsentence"})) {
            marker = QStringLiteral("{sentence}");
        } else if (among(name, {"sentenceaudio", "contextaudio"})) {
            marker = QStringLiteral("{audio}");
        } else if (IsWordAudioField(field)) {
            marker = QStringLiteral("{word-audio}");
        } else if (among(name, {"picture", "image", "screenshot", "sentenceimage", "img"})) {
            marker = QStringLiteral("{screenshot}");
        } else if (among(name, {"maindefinition", "meaning", "glossary", "definition",
                                "definitions", "vocabdef", "english", "selectiontext", "back"})) {
            marker = QStringLiteral("{glossary}");
        } else if (among(name, {"frequency", "frequencies", "freq"})) {
            marker = QStringLiteral("{frequencies}");
        } else if (among(name, {"freqsort", "frequencysort", "freqrank"})) {
            marker = QStringLiteral("{frequency-rank}");
        } else if (among(name, {"pitchposition", "pitchpositions", "pitchaccents", "pitch"})) {
            marker = QStringLiteral("{pitch-positions}");
        } else if (among(name, {"source", "miscinfo", "document", "sourcemedia", "game"})) {
            marker = QStringLiteral("{source}");
        } else if (among(name, {"url", "link", "sourceurl"})) {
            marker = QStringLiteral("{url}");
        }
        out.insert(field, marker);
    }
    return out;
}

} // namespace ReaderApp::CardBuilder
