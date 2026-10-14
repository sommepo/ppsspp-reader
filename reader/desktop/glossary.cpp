// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include "reader/desktop/glossary.h"
#include "reader/desktop/text_line.h"

namespace ReaderApp::Glossary {

namespace {

struct ListState {
    bool ordered{};
    int index{};
    int depth{};
};

QString Str(const char16_t* text) {
    return QString::fromUtf16(text);
}

bool IsOneOf(const QString& value, std::initializer_list<const char16_t*> options) {
    return std::any_of(options.begin(), options.end(),
                       [&value](const char16_t* option) { return value == Str(option); });
}

QString NumberText(const QJsonValue& value) {
    const double number = value.toDouble();
    if (number == std::floor(number)) {
        return QString::number(static_cast<qint64>(number));
    }
    return QString::number(number, 'g', 15);
}

/// What a style or data value says, as text.
QString ScalarText(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return NumberText(value);
    }
    if (value.isBool()) {
        return value.toBool() ? Str(u"true") : Str(u"false");
    }
    return {};
}

void AppendText(QString& out, const QString& text) {
    QString escaped = EscapeHtml(text);
    escaped.replace(u'\n', Str(u"<br>"));
    out += escaped;
}

/// Removes whitespace and line breaks from both ends of a fragment.
QString TrimBreaks(const QString& html) {
    QString text = html.trimmed();
    while (true) {
        QString shorter = text;
        if (shorter.startsWith(Str(u"<br>"))) {
            shorter = shorter.mid(4);
        }
        if (shorter.endsWith(Str(u"<br>"))) {
            shorter.chop(4);
        }
        shorter = shorter.trimmed();
        if (shorter == text) {
            return text;
        }
        text = shorter;
    }
}

void Render(const QJsonValue& node, Mode mode, QString& out, ListState& list);

QString StyleToCss(const QJsonObject& style) {
    static const QRegularExpression property_name(Str(u"^[a-z\\-]+$"));
    QString out;
    for (auto it = style.constBegin(); it != style.constEnd(); ++it) {
        if (it.value().isObject() || it.value().isArray() || it.value().isNull()) {
            continue;
        }
        QString property;
        for (const QChar c : it.key()) {
            if (c.unicode() >= u'A' && c.unicode() <= u'Z') {
                property += u'-';
                property += c.toLower();
            } else {
                property += c;
            }
        }
        if (!property_name.match(property).hasMatch()) {
            continue;
        }
        const QString text = ScalarText(it.value());
        if (text.contains(u';') || text.contains(u'{') || text.contains(u'}')) {
            continue;
        }
        out += property + Str(u": ") + text + Str(u"; ");
    }
    return out.trimmed();
}

void FullElement(const QString& tag, const QJsonObject& node, const QJsonValue& content,
                 QString& out, ListState& list) {
    static const QRegularExpression unsafe(Str(u"[^A-Za-z0-9_\\-]"));
    if (tag == Str(u"br")) {
        out += Str(u"<br>");
        return;
    }
    if (tag == Str(u"img")) {
        return;
    }
    if (!IsOneOf(tag, {u"ruby", u"rt", u"rp", u"table", u"thead", u"tbody", u"tfoot", u"tr", u"td",
                       u"th", u"span", u"div", u"ol", u"ul", u"li", u"details", u"summary", u"a"})) {
        Render(content, Mode::Full, out, list);
        return;
    }
    out += u'<' + tag;
    if (node.value(Str(u"style")).isObject()) {
        const QString css = StyleToCss(node.value(Str(u"style")).toObject());
        if (!css.isEmpty()) {
            out += Str(u" style=\"") + EscapeHtml(css) + u'"';
        }
    }
    if (node.value(Str(u"data")).isObject()) {
        const QJsonObject data = node.value(Str(u"data")).toObject();
        for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
            QString name = it.key();
            name.replace(unsafe, Str(u"-"));
            out += Str(u" data-sc-") + name + Str(u"=\"") + EscapeHtml(ScalarText(it.value())) +
                   u'"';
        }
    }
    for (const char16_t* attribute : {u"lang", u"title"}) {
        if (node.contains(Str(attribute))) {
            out += u' ' + Str(attribute) + Str(u"=\"") +
                   EscapeHtml(ScalarText(node.value(Str(attribute)))) + u'"';
        }
    }
    if (tag == Str(u"td") || tag == Str(u"th")) {
        for (const char16_t* attribute : {u"colSpan", u"rowSpan"}) {
            if (node.contains(Str(attribute))) {
                out += u' ' + Str(attribute).toLower() + Str(u"=\"") +
                       QString::number(node.value(Str(attribute)).toInt(1)) + u'"';
            }
        }
    }
    if (tag == Str(u"a")) {
        const QString href = node.value(Str(u"href")).toString();
        // Only real links are useful outside the dictionary viewer.
        if (href.startsWith(Str(u"http://")) || href.startsWith(Str(u"https://"))) {
            out += Str(u" href=\"") + EscapeHtml(href) + u'"';
        }
    }
    out += u'>';
    Render(content, Mode::Full, out, list);
    out += Str(u"</") + tag + u'>';
}

/// Separates blocks with exactly one line break.
void Block(QString& out) {
    if (out.isEmpty()) {
        return;
    }
    if (!out.endsWith(Str(u"<br>"))) {
        out += Str(u"<br>");
    }
}

void Styled(const QJsonObject& node, const QJsonValue& content, QString& out, ListState& list) {
    const QJsonObject style = node.value(Str(u"style")).toObject();
    const bool bold = style.value(Str(u"fontWeight")).toString() == Str(u"bold");
    const bool italic = style.value(Str(u"fontStyle")).toString() == Str(u"italic");
    const bool underline =
        style.value(Str(u"textDecorationLine")).toString().contains(Str(u"underline"));
    bool small = false;
    const QString size = style.value(Str(u"fontSize")).toString();
    if (!size.isEmpty()) {
        if (size == Str(u"small") || size == Str(u"x-small")) {
            small = true;
        } else {
            QString number = size;
            if (number.endsWith(Str(u"em"))) {
                number.chop(2);
            }
            bool ok = false;
            const double scale = number.toDouble(&ok);
            small = ok && scale < 0.9;
        }
    }
    if (bold) {
        out += Str(u"<b>");
    }
    if (italic) {
        out += Str(u"<i>");
    }
    if (underline) {
        out += Str(u"<u>");
    }
    if (small) {
        out += Str(u"<small>");
    }
    Render(content, Mode::Simple, out, list);
    if (small) {
        out += Str(u"</small>");
    }
    if (underline) {
        out += Str(u"</u>");
    }
    if (italic) {
        out += Str(u"</i>");
    }
    if (bold) {
        out += Str(u"</b>");
    }
}

void SimpleElement(const QString& tag, const QJsonObject& node, const QJsonValue& content,
                   QString& out, ListState& list) {
    if (tag == Str(u"br")) {
        out += Str(u"<br>");
    } else if (IsOneOf(tag, {u"img", u"rt", u"rp"})) {
        // left out
    } else if (tag == Str(u"ruby")) {
        Render(content, Mode::Simple, out, list);
    } else if (tag == Str(u"div") || tag == Str(u"details")) {
        Block(out);
        Styled(node, content, out, list);
        Block(out);
    } else if (tag == Str(u"summary")) {
        out += Str(u"<b>");
        Render(content, Mode::Simple, out, list);
        out += Str(u"</b>");
        Block(out);
    } else if (tag == Str(u"ol") || tag == Str(u"ul")) {
        ListState inner{tag == Str(u"ol"), 0, list.depth + 1};
        Block(out);
        Render(content, Mode::Simple, out, inner);
        Block(out);
    } else if (tag == Str(u"li")) {
        Block(out);
        for (int i = 0; i < std::max(list.depth - 1, 0); ++i) {
            out += Str(u"&nbsp;&nbsp;");
        }
        ++list.index;
        out += list.ordered ? QString::number(list.index) + Str(u". ") : Str(u"• ");
        ListState inner{list.ordered, 0, list.depth};
        Styled(node, content, out, inner);
    } else if (IsOneOf(tag, {u"table", u"thead", u"tbody", u"tfoot"})) {
        Render(content, Mode::Simple, out, list);
    } else if (tag == Str(u"tr")) {
        Block(out);
        Render(content, Mode::Simple, out, list);
        Block(out);
    } else if (tag == Str(u"td") || tag == Str(u"th")) {
        if (tag == Str(u"th")) {
            out += Str(u"<b>");
        }
        Render(content, Mode::Simple, out, list);
        if (tag == Str(u"th")) {
            out += Str(u"</b>");
        }
        out += Str(u" &nbsp;");
    } else {
        Styled(node, content, out, list);
    }
}

void Render(const QJsonValue& node, Mode mode, QString& out, ListState& list) {
    switch (node.type()) {
    case QJsonValue::String:
        AppendText(out, node.toString());
        break;
    case QJsonValue::Double:
        out += NumberText(node);
        break;
    case QJsonValue::Bool:
        out += node.toBool() ? Str(u"true") : Str(u"false");
        break;
    case QJsonValue::Array:
        for (const QJsonValue& child : node.toArray()) {
            Render(child, mode, out, list);
        }
        break;
    case QJsonValue::Object: {
        const QJsonObject object = node.toObject();
        const QString tag = object.value(Str(u"tag")).toString();
        const QJsonValue content = object.value(Str(u"content"));
        if (mode == Mode::Full) {
            FullElement(tag, object, content, out, list);
        } else {
            SimpleElement(tag, object, content, out, list);
        }
        break;
    }
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        break;
    }
}

/// The text of structured content, one line per block.
void Plain(const QJsonValue& node, QString& out) {
    switch (node.type()) {
    case QJsonValue::String:
        out += node.toString();
        break;
    case QJsonValue::Double:
        out += NumberText(node);
        break;
    case QJsonValue::Bool:
        out += node.toBool() ? Str(u"true") : Str(u"false");
        break;
    case QJsonValue::Array:
        for (const QJsonValue& child : node.toArray()) {
            Plain(child, out);
        }
        break;
    case QJsonValue::Object: {
        const QJsonObject object = node.toObject();
        const QString tag = object.value(Str(u"tag")).toString();
        if (tag == Str(u"br")) {
            out += u'\n';
        } else if (IsOneOf(tag, {u"img", u"rt", u"rp"})) {
            // left out
        } else if (IsOneOf(tag, {u"li", u"div", u"p", u"ul", u"ol", u"tr", u"table", u"details",
                                 u"summary", u"thead", u"tbody", u"tfoot"})) {
            out += u'\n';
            Plain(object.value(Str(u"content")), out);
            out += u'\n';
        } else {
            Plain(object.value(Str(u"content")), out);
            if (tag == Str(u"td") || tag == Str(u"th")) {
                out += u' ';
            }
        }
        break;
    }
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        break;
    }
}

QString PlainText(const QJsonValue& node) {
    QString out;
    Plain(node, out);
    return out;
}

/// Lines run together with "; ", without the space around them.
QString OneLine(const QString& text) {
    QStringList parts;
    for (const QString& line : text.split(u'\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            parts.append(trimmed);
        }
    }
    return parts.join(Str(u"; "));
}

/// Finds the parts a dictionary marked as glosses or as notes, wherever they are nested.
void CollectMarked(const QJsonValue& node, QList<QJsonValue>& glosses, QList<QJsonValue>& notes) {
    if (node.isArray()) {
        for (const QJsonValue& child : node.toArray()) {
            CollectMarked(child, glosses, notes);
        }
    } else if (node.isObject()) {
        const QJsonObject object = node.toObject();
        const QString mark =
            object.value(Str(u"data")).toObject().value(Str(u"content")).toString();
        if (mark == Str(u"glossary")) {
            glosses.append(node);
        } else if (IsOneOf(mark, {u"notes", u"infoGlossary", u"sourceLanguages"})) {
            notes.append(node);
        } else if (mark.isEmpty()) {
            CollectMarked(object.value(Str(u"content")), glosses, notes);
        }
        // Anything else is marked as something the compact popup leaves out.
    }
}

Compact CompactContent(const QJsonValue& content) {
    QList<QJsonValue> glosses;
    QList<QJsonValue> notes;
    CollectMarked(content, glosses, notes);
    if (glosses.isEmpty()) {
        return Compact{OneLine(PlainText(content)), {}};
    }
    const auto join = [](const QList<QJsonValue>& nodes) {
        QStringList parts;
        for (const QJsonValue& node : nodes) {
            parts.append(OneLine(PlainText(node)));
        }
        return parts.join(Str(u"; "));
    };
    return Compact{join(glosses), join(notes)};
}

QJsonArray ParseArray(const QString& glossary_json, bool& ok) {
    const QJsonDocument document = QJsonDocument::fromJson(glossary_json.toUtf8());
    ok = document.isArray();
    return document.array();
}

} // namespace

QStringList Definitions(const QString& glossary_json, Mode mode) {
    bool ok = false;
    const QJsonArray array = ParseArray(glossary_json, ok);
    QStringList out;
    if (!ok) {
        return out;
    }
    for (const QJsonValue& item : array) {
        QString html;
        if (item.isString()) {
            AppendText(html, item.toString());
        } else if (item.isObject()) {
            const QJsonObject object = item.toObject();
            const QString type = object.value(Str(u"type")).toString();
            if (type == Str(u"text")) {
                AppendText(html, object.value(Str(u"text")).toString());
            } else if (type == Str(u"structured-content")) {
                ListState list;
                Render(object.value(Str(u"content")), mode, html, list);
            }
            // images are left out
        }
        // deinflection pointers [term, [reasons]] carry no definition
        const QString text = TrimBreaks(html);
        if (!text.isEmpty()) {
            out.append(text);
        }
    }
    return out;
}

QString ToHtml(const QString& glossary_json, Mode mode) {
    const QStringList items = Definitions(glossary_json, mode);
    if (items.isEmpty()) {
        return {};
    }
    if (items.size() == 1) {
        return items.first();
    }
    QString out;
    if (mode == Mode::Full) {
        out += Str(u"<ul>");
        for (const QString& item : items) {
            out += Str(u"<li>") + item + Str(u"</li>");
        }
        out += Str(u"</ul>");
    } else {
        QStringList bullets;
        for (const QString& item : items) {
            bullets.append(Str(u"• ") + item);
        }
        out = bullets.join(Str(u"<br>"));
    }
    return out;
}

QString StripTags(const QString& html) {
    static const QRegularExpression breaks(Str(u"<br\\s*/?>|</div>|</p>"));
    static const QRegularExpression tags(Str(u"<[^>]+>"));
    static const QRegularExpression blank_lines(Str(u"\n{2,}"));
    QString out = html;
    out.replace(breaks, Str(u"\n"));
    out.replace(tags, QString());
    out.replace(Str(u"&lt;"), Str(u"<"));
    out.replace(Str(u"&gt;"), Str(u">"));
    out.replace(Str(u"&quot;"), Str(u"\""));
    out.replace(Str(u"&amp;"), Str(u"&"));
    out.replace(blank_lines, Str(u"\n"));
    return out.trimmed();
}

QString ToPlainText(const QString& glossary_json) {
    QStringList parts;
    for (const QString& definition : Definitions(glossary_json, Mode::Simple)) {
        parts.append(StripTags(definition));
    }
    return parts.join(Str(u"; "));
}

QList<Compact> CompactDefinitions(const QString& glossary_json) {
    bool ok = false;
    const QJsonArray array = ParseArray(glossary_json, ok);
    QList<Compact> out;
    if (!ok) {
        return out;
    }
    for (const QJsonValue& item : array) {
        Compact definition;
        if (item.isString()) {
            definition = Compact{OneLine(item.toString()), {}};
        } else if (item.isObject()) {
            const QJsonObject object = item.toObject();
            const QString type = object.value(Str(u"type")).toString();
            if (type == Str(u"text")) {
                definition = Compact{OneLine(object.value(Str(u"text")).toString()), {}};
            } else if (type == Str(u"structured-content")) {
                definition = CompactContent(object.value(Str(u"content")));
            }
        }
        if (!definition.glosses.isEmpty()) {
            out.append(definition);
        }
    }
    return out;
}

} // namespace ReaderApp::Glossary
