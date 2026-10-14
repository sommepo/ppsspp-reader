// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <initializer_list>
#include <tuple>
#include <utility>
#include "reader/desktop/deinflector.h"

namespace ReaderApp {

int WordClass::Parse(const QString& rules) {
    if (rules.isEmpty()) {
        return 0;
    }
    int flags = 0;
    for (const QString& rule : rules.split(u' ')) {
        if (rule.startsWith(QStringLiteral("v1"))) {
            flags |= V1;
        } else if (rule.startsWith(QStringLiteral("v5"))) {
            flags |= V5;
        } else if (rule.startsWith(QStringLiteral("vs"))) {
            flags |= VS;
        } else if (rule == QStringLiteral("vk")) {
            flags |= VK;
        } else if (rule == QStringLiteral("vz")) {
            flags |= VZ;
        } else if (rule == QStringLiteral("adj-i") || rule == QStringLiteral("adj-ix")) {
            flags |= ADJ_I;
        }
    }
    return flags;
}

namespace {

constexpr int MaxCandidates = 400;
constexpr int MaxDepth = 6;

// Godan verbs by dictionary ending.
const QString U = QStringLiteral("うくぐすつぬぶむる");
const QString A = QStringLiteral("わかがさたなばまら"); // negative stem
const QString I = QStringLiteral("いきぎしちにびみり"); // masu stem
const QString E = QStringLiteral("えけげせてねべめれ"); // potential / imperative / conditional
const QString O = QStringLiteral("おこごそとのぼもろ"); // volitional stem
const QStringList TeForm = {QStringLiteral("って"), QStringLiteral("いて"), QStringLiteral("いで"),
                            QStringLiteral("して"), QStringLiteral("って"), QStringLiteral("んで"),
                            QStringLiteral("んで"), QStringLiteral("んで"), QStringLiteral("って")};

QString Key(const QString& term, int conditions) {
    return term + QChar(u'\0') + QString::number(conditions);
}

QString S(const char16_t* text) {
    return QString::fromUtf16(text);
}

} // namespace

Deinflector::Deinflector() {
    using namespace WordClass;
    const auto rule = [this](const QString& reason, const QString& from, const QString& to,
                             int class_in, int class_out) {
        rules.append(Rule{reason, from, to, class_in, class_out});
    };
    // Adds `reason` for all godan endings: stem row + `suffix` goes back to the u row.
    const auto godan = [&rule](const QString& reason, const QString& row, const QString& suffix,
                               int class_in) {
        for (int i = 0; i < U.size(); ++i) {
            rule(reason, row.at(i) + suffix, QString(U.at(i)), class_in, V5);
        }
    };
    // Adds `reason` for forms built on the te/ta sound change, e.g. った / んだ.
    const auto godan_euphonic = [&rule](const QString& reason,
                                        const std::function<QString(const QString&)>& replace,
                                        int class_in) {
        for (int i = 0; i < U.size(); ++i) {
            rule(reason, replace(TeForm.at(i)), QString(U.at(i)), class_in, V5);
        }
        // 行く is the one godan verb in く with って.
        rule(reason, replace(S(u"行って")), S(u"行く"), class_in, V5);
        rule(reason, replace(S(u"いって")), S(u"いく"), class_in, V5);
    };
    // する, くる and their kanji forms: `suru` and `kuru` are the inflected spellings.
    const auto irregular = [&rule](const QString& reason, int class_in, const QString& suru,
                                   const QString& kuru) {
        if (!suru.isNull()) {
            rule(reason, suru, S(u"する"), class_in, VS);
            // じる / ずる verbs (感じる, 論ずる) share the し-stem pattern.
            if (suru.startsWith(u'し')) {
                rule(reason, u'じ' + suru.mid(1), S(u"ずる"), class_in, VZ);
            }
        }
        if (!kuru.isNull()) {
            rule(reason, kuru, S(u"くる"), class_in, VK);
            rule(reason, u'来' + kuru.mid(1), S(u"来る"), class_in, VK);
        }
    };
    // The te-form with its last kana replaced: って → った, んで → んだ.
    const auto ending = [](const QString& voiceless, const QString& voiced) {
        return [voiceless, voiced](const QString& te) {
            return te.left(te.size() - 1) + (te.endsWith(u'で') ? voiced : voiceless);
        };
    };
    // ---- Negative: ない behaves as an i-adjective ----
    rule(S(u"negative"), S(u"ない"), S(u"る"), ADJ_I, V1);
    godan(S(u"negative"), A, S(u"ない"), ADJ_I);
    irregular(S(u"negative"), ADJ_I, S(u"しない"), S(u"こない"));
    rule(S(u"negative"), S(u"くない"), S(u"い"), ADJ_I, ADJ_I);
    rule(S(u"negative"), S(u"ない"), S(u"ある"), ADJ_I, V5); // ある has the negative ない

    // ---- Past ----
    rule(S(u"past"), S(u"た"), S(u"る"), 0, V1);
    godan_euphonic(S(u"past"), ending(S(u"た"), S(u"だ")), 0);
    irregular(S(u"past"), 0, S(u"した"), S(u"きた"));
    rule(S(u"past"), S(u"かった"), S(u"い"), 0, ADJ_I);

    // ---- Te-form ----
    rule(S(u"-te"), S(u"て"), S(u"る"), TE, V1);
    godan_euphonic(S(u"-te"), [](const QString& te) { return te; }, TE);
    irregular(S(u"-te"), TE, S(u"して"), S(u"きて"));
    rule(S(u"-te"), S(u"くて"), S(u"い"), TE, ADJ_I);
    rule(S(u"-te"), S(u"ないで"), S(u"ない"), TE, ADJ_I);

    // ---- Progressive / perfect: built on the te-form, behaves as ichidan ----
    for (const auto& [from, to] : std::initializer_list<std::pair<QString, QString>>{
             {S(u"ている"), S(u"て")},
             {S(u"ておる"), S(u"て")},
             {S(u"てる"), S(u"て")},
             {S(u"とる"), S(u"て")},
             {S(u"でいる"), S(u"で")},
             {S(u"でおる"), S(u"で")},
             {S(u"でる"), S(u"で")},
             {S(u"どる"), S(u"で")}}) {
        rule(S(u"progressive or perfect"), from, to, V1, TE);
    }

    // ---- Auxiliaries on the te-form ----
    rule(S(u"-shimau"), S(u"てしまう"), S(u"て"), V5, TE);
    rule(S(u"-shimau"), S(u"でしまう"), S(u"で"), V5, TE);
    rule(S(u"-oku"), S(u"ておく"), S(u"て"), V5, TE);
    rule(S(u"-oku"), S(u"でおく"), S(u"で"), V5, TE);

    // ---- Contractions of てしまう and ておく ----
    for (const auto& [name, aux, voiced] :
         std::initializer_list<std::tuple<QString, QString, QString>>{
             {S(u"-chau"), S(u"ちゃう"), S(u"じゃう")},
             {S(u"-chimau"), S(u"ちまう"), S(u"じまう")}}) {
        rule(name, aux, S(u"る"), V5, V1);
        godan_euphonic(name, ending(aux, voiced), V5);
        irregular(name, V5, u'し' + aux, u'き' + aux);
    }
    rule(S(u"-toku"), S(u"とく"), S(u"る"), V5, V1);
    godan_euphonic(S(u"-toku"), ending(S(u"とく"), S(u"どく")), V5);
    irregular(S(u"-toku"), V5, S(u"しとく"), S(u"きとく"));

    // ---- Polite ----
    for (const auto& [name, suffix] : std::initializer_list<std::pair<QString, QString>>{
             {S(u"polite"), S(u"ます")},
             {S(u"polite negative"), S(u"ません")},
             {S(u"polite past"), S(u"ました")},
             {S(u"polite past negative"), S(u"ませんでした")},
             {S(u"polite volitional"), S(u"ましょう")}}) {
        rule(name, suffix, S(u"る"), 0, V1);
        godan(name, I, suffix, 0);
        irregular(name, 0, u'し' + suffix, u'き' + suffix);
    }
    rule(S(u"polite negative"), S(u"くありません"), S(u"い"), 0, ADJ_I);
    rule(S(u"polite past negative"), S(u"くありませんでした"), S(u"い"), 0, ADJ_I);

    // ---- Desire: たい behaves as an i-adjective ----
    rule(S(u"-tai"), S(u"たい"), S(u"る"), ADJ_I, V1);
    godan(S(u"-tai"), I, S(u"たい"), ADJ_I);
    irregular(S(u"-tai"), ADJ_I, S(u"したい"), S(u"きたい"));

    // ---- Volitional ----
    rule(S(u"volitional"), S(u"よう"), S(u"る"), 0, V1);
    for (int i = 0; i < U.size(); ++i) {
        rule(S(u"volitional"), O.at(i) + S(u"う"), QString(U.at(i)), 0, V5);
    }
    irregular(S(u"volitional"), 0, S(u"しよう"), S(u"こよう"));
    rule(S(u"volitional"), S(u"かろう"), S(u"い"), 0, ADJ_I);

    // ---- Imperative ----
    rule(S(u"imperative"), S(u"ろ"), S(u"る"), 0, V1);
    rule(S(u"imperative"), S(u"よ"), S(u"る"), 0, V1);
    for (int i = 0; i < U.size(); ++i) {
        rule(S(u"imperative"), QString(E.at(i)), QString(U.at(i)), 0, V5);
    }
    rule(S(u"imperative"), S(u"しろ"), S(u"する"), 0, VS);
    rule(S(u"imperative"), S(u"せよ"), S(u"する"), 0, VS);
    rule(S(u"imperative"), S(u"こい"), S(u"くる"), 0, VK);
    rule(S(u"imperative"), S(u"来い"), S(u"来る"), 0, VK);
    rule(S(u"imperative negative"), S(u"な"), QStringLiteral(""), 0, ANY_VERB);

    // ---- Request: なさい ----
    rule(S(u"-nasai"), S(u"なさい"), S(u"る"), 0, V1);
    godan(S(u"-nasai"), I, S(u"なさい"), 0);
    irregular(S(u"-nasai"), 0, S(u"しなさい"), S(u"きなさい"));

    // ---- Potential, passive, causative: all behave as ichidan verbs ----
    rule(S(u"potential or passive"), S(u"られる"), S(u"る"), V1, V1);
    rule(S(u"potential"), S(u"れる"), S(u"る"), V1, V1); // colloquial 食べれる
    for (int i = 0; i < U.size(); ++i) {
        rule(S(u"potential"), E.at(i) + S(u"る"), QString(U.at(i)), V1, V5);
    }
    godan(S(u"passive"), A, S(u"れる"), V1);
    rule(S(u"passive"), S(u"される"), S(u"する"), V1, VS);
    rule(S(u"potential or passive"), S(u"こられる"), S(u"くる"), V1, VK);
    rule(S(u"potential or passive"), S(u"来られる"), S(u"来る"), V1, VK);
    rule(S(u"potential"), S(u"これる"), S(u"くる"), V1, VK);
    rule(S(u"potential"), S(u"来れる"), S(u"来る"), V1, VK);
    rule(S(u"causative"), S(u"させる"), S(u"る"), V1, V1);
    godan(S(u"causative"), A, S(u"せる"), V1);
    rule(S(u"causative"), S(u"させる"), S(u"する"), V1, VS);
    rule(S(u"causative"), S(u"こさせる"), S(u"くる"), V1, VK);
    rule(S(u"causative"), S(u"来させる"), S(u"来る"), V1, VK);
    godan(S(u"causative"), A, S(u"す"), V5); // short causative 読ます
    godan(S(u"causative passive"), A, S(u"される"), V1);
    rule(S(u"causative passive"), S(u"させられる"), S(u"る"), V1, V1);
    rule(S(u"causative passive"), S(u"させられる"), S(u"する"), V1, VS);

    // ---- Conditionals ----
    rule(S(u"-ba"), S(u"れば"), S(u"る"), 0, V1);
    for (int i = 0; i < U.size(); ++i) {
        rule(S(u"-ba"), E.at(i) + S(u"ば"), QString(U.at(i)), 0, V5);
    }
    rule(S(u"-ba"), S(u"すれば"), S(u"する"), 0, VS);
    rule(S(u"-ba"), S(u"くれば"), S(u"くる"), 0, VK);
    rule(S(u"-ba"), S(u"来れば"), S(u"来る"), 0, VK);
    rule(S(u"-ba"), S(u"ければ"), S(u"い"), 0, ADJ_I);
    rule(S(u"-ba"), S(u"きゃ"), S(u"い"), 0, ADJ_I); // なきゃ, colloquial ければ
    rule(S(u"-ba"), S(u"けりゃ"), S(u"い"), 0, ADJ_I);
    rule(S(u"-tara"), S(u"たら"), S(u"る"), 0, V1);
    godan_euphonic(S(u"-tara"), ending(S(u"たら"), S(u"だら")), 0);
    irregular(S(u"-tara"), 0, S(u"したら"), S(u"きたら"));
    rule(S(u"-tara"), S(u"かったら"), S(u"い"), 0, ADJ_I);
    rule(S(u"-tari"), S(u"たり"), S(u"る"), 0, V1);
    godan_euphonic(S(u"-tari"), ending(S(u"たり"), S(u"だり")), 0);
    irregular(S(u"-tari"), 0, S(u"したり"), S(u"きたり"));
    rule(S(u"-tari"), S(u"かったり"), S(u"い"), 0, ADJ_I);

    // ---- ては contractions ----
    rule(S(u"-cha"), S(u"ちゃ"), S(u"る"), 0, V1);
    godan_euphonic(S(u"-cha"), ending(S(u"ちゃ"), S(u"じゃ")), 0);
    irregular(S(u"-cha"), 0, S(u"しちゃ"), S(u"きちゃ"));
    rule(S(u"-cha"), S(u"くちゃ"), S(u"い"), 0, ADJ_I);

    // ---- Classical and colloquial negatives ----
    for (const auto& [name, suffix] : std::initializer_list<std::pair<QString, QString>>{
             {S(u"-zu"), S(u"ず")}, {S(u"-nu"), S(u"ぬ")}, {S(u"-n"), S(u"ん")}}) {
        rule(name, suffix, S(u"る"), 0, V1);
        godan(name, A, suffix, 0);
        rule(name, u'せ' + suffix, S(u"する"), 0, VS);
        rule(name, u'こ' + suffix, S(u"くる"), 0, VK);
        rule(name, u'来' + suffix, S(u"来る"), 0, VK);
    }
    for (const QString& slang : {S(u"ねえ"), S(u"ねぇ"), S(u"ねー")}) {
        rule(S(u"slang"), slang, S(u"ない"), 0, ADJ_I);
    }
    for (const QString& slang : {S(u"てえ"), S(u"てぇ"), S(u"てー")}) {
        rule(S(u"slang"), slang, S(u"たい"), 0, ADJ_I);
    }

    // ---- Appearance and excess ----
    rule(S(u"-sou"), S(u"そう"), S(u"る"), 0, V1);
    godan(S(u"-sou"), I, S(u"そう"), 0);
    irregular(S(u"-sou"), 0, S(u"しそう"), S(u"きそう"));
    rule(S(u"-sou"), S(u"そう"), S(u"い"), 0, ADJ_I);
    for (const QString& sugiru : {S(u"すぎる"), S(u"過ぎる")}) {
        rule(S(u"-sugiru"), sugiru, S(u"る"), V1, V1);
        godan(S(u"-sugiru"), I, sugiru, V1);
        irregular(S(u"-sugiru"), V1, u'し' + sugiru, u'き' + sugiru);
        rule(S(u"-sugiru"), sugiru, S(u"い"), V1, ADJ_I);
    }

    // ---- Stems and adjective derivations ----
    rule(S(u"masu stem"), QStringLiteral(""), S(u"る"), 0, V1);
    for (int i = 0; i < U.size(); ++i) {
        rule(S(u"masu stem"), QString(I.at(i)), QString(U.at(i)), 0, V5);
    }
    rule(S(u"masu stem"), S(u"し"), S(u"する"), 0, VS);
    rule(S(u"masu stem"), S(u"き"), S(u"くる"), 0, VK);
    rule(S(u"masu stem"), S(u"来"), S(u"来る"), 0, VK);
    rule(S(u"adverb"), S(u"く"), S(u"い"), 0, ADJ_I);
    rule(S(u"noun"), S(u"さ"), S(u"い"), 0, ADJ_I);
    rule(S(u"-ki"), S(u"き"), S(u"い"), 0, ADJ_I);
    rule(S(u"-ge"), S(u"げ"), S(u"い"), 0, ADJ_I);
    rule(S(u"-garu"), S(u"がる"), S(u"い"), V5, ADJ_I);

    for (int i = 0; i < rules.size(); ++i) {
        const Rule& r = rules.at(i);
        if (r.suffix_in.isEmpty()) {
            unconditional.append(i);
        } else {
            by_last_char[r.suffix_in.back()].append(i);
        }
    }
}

QList<Deinflection> Deinflector::Deinflect(const QString& source) const {
    QList<Deinflection> results;
    QSet<QString> seen;
    results.append(Deinflection{source, 0, {}});
    seen.insert(Key(source, 0));
    int index = 0;
    while (index < results.size() && results.size() < MaxCandidates) {
        // A copy: the list grows while it is walked.
        const Deinflection current = results.at(index++);
        if (current.term.isEmpty() || current.reasons.size() >= MaxDepth) {
            continue;
        }
        const auto candidates = by_last_char.constFind(current.term.back());
        if (candidates != by_last_char.constEnd()) {
            Apply(*candidates, current, results, seen);
        }
        Apply(unconditional, current, results, seen);
    }
    return results;
}

void Deinflector::Apply(const QList<int>& candidates, const Deinflection& current,
                        QList<Deinflection>& results, QSet<QString>& seen) const {
    for (const int rule_index : candidates) {
        const Rule& rule = rules.at(rule_index);
        if (current.conditions != 0 && (current.conditions & rule.class_in) == 0) {
            continue;
        }
        if (current.conditions == 0 && !current.reasons.isEmpty()) {
            continue;
        }
        if (!current.term.endsWith(rule.suffix_in)) {
            continue;
        }
        const int stem_length = static_cast<int>(current.term.size() - rule.suffix_in.size());
        if (stem_length + rule.suffix_out.size() <= 0) {
            continue;
        }
        // A masu-stem guess must not grow out of nothing or repeat forever.
        if (rule.suffix_in.isEmpty() && !current.reasons.isEmpty()) {
            continue;
        }
        const QString term = current.term.left(stem_length) + rule.suffix_out;
        const QString key = Key(term, rule.class_out);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        if (results.size() >= MaxCandidates) {
            return;
        }
        QStringList reasons = current.reasons;
        reasons.append(rule.reason);
        results.append(Deinflection{term, rule.class_out, reasons});
    }
}

} // namespace ReaderApp
