// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

package org.ppsspp.ppsspp.reader.dict

/** A possible dictionary form of some text, with the inflections that were undone to reach it. */
data class Deinflection(
    val term: String,
    /** Word classes the dictionary entry must have ([WordClass] bits); 0 accepts any entry. */
    val conditions: Int,
    /** Inflection names, outermost first: 食べさせられた gives [past, passive, causative]. */
    val reasons: List<String>
)

/** Word classes as dictionaries tag them in a term's "rules" field. */
object WordClass {
    const val V1 = 1 // ichidan verb
    const val V5 = 2 // godan verb
    const val VS = 4 // suru verb
    const val VK = 8 // kuru verb
    const val VZ = 16 // zuru verb
    const val ADJ_I = 32 // i-adjective

    /** Not a dictionary class: the te-form, so that 食べている can pass through 食べて. */
    const val TE = 64

    const val ANY_VERB = V1 or V5 or VS or VK or VZ

    /** Parses a term's space-separated rules ("v1 vt", "v5", "adj-i", "vs"...). */
    fun parse(rules: String): Int {
        if (rules.isEmpty()) return 0
        var flags = 0
        for (rule in rules.split(' ')) {
            flags = flags or when {
                rule == "v1" || rule.startsWith("v1") -> V1
                rule == "v5" || rule.startsWith("v5") -> V5
                rule == "vs" || rule.startsWith("vs") -> VS
                rule == "vk" -> VK
                rule == "vz" -> VZ
                rule == "adj-i" || rule == "adj-ix" -> ADJ_I
                else -> 0
            }
        }
        return flags
    }
}

/**
 * Undoes Japanese inflection by suffix rewriting: every rule replaces an inflected ending with
 * the ending of the form it was built from, and rules chain until nothing applies. The results
 * are candidates only; a candidate counts once a dictionary has that term with a matching word
 * class.
 *
 * Each rule says which class the inflected form itself behaves as ([Rule.classIn]) and which
 * class the result must be ([Rule.classOut]). A rule is tried on the original text and on any
 * candidate whose required class overlaps its [Rule.classIn]; a rule with classIn 0 is only
 * tried on the original text (the form cannot be inflected further).
 */
class Deinflector {
    private class Rule(
        val reason: String,
        val suffixIn: String,
        val suffixOut: String,
        val classIn: Int,
        val classOut: Int
    )

    private val rules: List<Rule> = buildRules()

    // Rules indexed by the last character of their inflected suffix; "" suffixes apply to all.
    private val byLastChar: Map<Char, List<Rule>> =
        rules.filter { it.suffixIn.isNotEmpty() }.groupBy { it.suffixIn.last() }
    private val unconditional: List<Rule> = rules.filter { it.suffixIn.isEmpty() }

    /** All candidates for [source], the unchanged text first. */
    fun deinflect(source: String): List<Deinflection> {
        val results = ArrayList<Deinflection>()
        val seen = HashSet<String>()
        results.add(Deinflection(source, 0, emptyList()))
        seen.add("$source\u00000")
        var index = 0
        while (index < results.size && results.size < MAX_CANDIDATES) {
            val current = results[index++]
            if (current.term.isEmpty() || current.reasons.size >= MAX_DEPTH) continue
            val candidates = byLastChar[current.term.last()]
            if (candidates != null) apply(candidates, current, results, seen)
            apply(unconditional, current, results, seen)
        }
        return results
    }

    private fun apply(
        candidates: List<Rule>,
        current: Deinflection,
        results: MutableList<Deinflection>,
        seen: MutableSet<String>
    ) {
        for (rule in candidates) {
            if (current.conditions != 0 && (current.conditions and rule.classIn) == 0) continue
            if (current.conditions == 0 && current.reasons.isNotEmpty()) continue
            if (!current.term.endsWith(rule.suffixIn)) continue
            val stemLength = current.term.length - rule.suffixIn.length
            if (stemLength + rule.suffixOut.length <= 0) continue
            // A masu-stem guess must not grow out of nothing or repeat forever.
            if (rule.suffixIn.isEmpty() && current.reasons.isNotEmpty()) continue
            val term = current.term.substring(0, stemLength) + rule.suffixOut
            val key = "$term\u0000${rule.classOut}"
            if (!seen.add(key)) continue
            if (results.size >= MAX_CANDIDATES) return
            val reasons = ArrayList<String>(current.reasons.size + 1)
            reasons.addAll(current.reasons)
            reasons.add(rule.reason)
            results.add(Deinflection(term, rule.classOut, reasons))
        }
    }

    private companion object {
        const val MAX_CANDIDATES = 400
        const val MAX_DEPTH = 6

        // Godan verbs by dictionary ending.
        const val U = "うくぐすつぬぶむる"
        const val A = "わかがさたなばまら" // negative stem
        const val I = "いきぎしちにびみり" // masu stem
        const val E = "えけげせてねべめれ" // potential / imperative / conditional stem
        const val O = "おこごそとのぼもろ" // volitional stem
        val TE_FORM = listOf("って", "いて", "いで", "して", "って", "んで", "んで", "んで", "って")
        val TA_FORM = listOf("った", "いた", "いだ", "した", "った", "んだ", "んだ", "んだ", "った")

        fun buildRules(): List<Rule> {
            val rules = ArrayList<Rule>()
            fun rule(reason: String, from: String, to: String, classIn: Int, classOut: Int) {
                rules.add(Rule(reason, from, to, classIn, classOut))
            }
            val v1 = WordClass.V1
            val v5 = WordClass.V5
            val vs = WordClass.VS
            val vk = WordClass.VK
            val vz = WordClass.VZ
            val adj = WordClass.ADJ_I
            val te = WordClass.TE

            /** Adds [reason] for all godan endings: stem row + [suffix] goes back to the u row. */
            fun godan(reason: String, row: String, suffix: String, classIn: Int) {
                for (i in U.indices) {
                    rule(reason, row[i] + suffix, U[i].toString(), classIn, v5)
                }
            }

            /** Adds [reason] for forms built on the te/ta sound change, e.g. った / んだ. */
            fun godanEuphonic(reason: String, replace: (String) -> String, classIn: Int) {
                for (i in U.indices) {
                    rule(reason, replace(TE_FORM[i]), U[i].toString(), classIn, v5)
                }
                // 行く is the one godan verb in く with って.
                rule(reason, replace("行って"), "行く", classIn, v5)
                rule(reason, replace("いって"), "いく", classIn, v5)
            }

            /** する, くる and their kanji forms: [suru] etc. are the inflected spellings. */
            fun irregular(reason: String, classIn: Int, suru: String?, kuru: String?) {
                if (suru != null) {
                    rule(reason, suru, "する", classIn, vs)
                    // じる / ずる verbs (感じる, 論ずる) share the し-stem pattern.
                    if (suru.startsWith("し")) {
                        rule(reason, "じ" + suru.substring(1), "ずる", classIn, vz)
                    }
                }
                if (kuru != null) {
                    rule(reason, kuru, "くる", classIn, vk)
                    val kanji = "来" + kuru.substring(1)
                    rule(reason, kanji, "来る", classIn, vk)
                }
            }

            // ---- Negative: ない behaves as an i-adjective ----
            rule("negative", "ない", "る", adj, v1)
            godan("negative", A, "ない", adj)
            irregular("negative", adj, "しない", "こない")
            rule("negative", "くない", "い", adj, adj)
            rule("negative", "ない", "ある", adj, v5) // ある has the suppletive negative ない

            // ---- Past ----
            rule("past", "た", "る", 0, v1)
            godanEuphonic("past", { it.dropLast(1) + if (it.endsWith("で")) "だ" else "た" }, 0)
            irregular("past", 0, "した", "きた")
            rule("past", "かった", "い", 0, adj)

            // ---- Te-form ----
            rule("-te", "て", "る", te, v1)
            godanEuphonic("-te", { it }, te)
            irregular("-te", te, "して", "きて")
            rule("-te", "くて", "い", te, adj)
            rule("-te", "ないで", "ない", te, adj)

            // ---- Progressive / perfect: built on the te-form, behaves as ichidan ----
            for ((from, to) in listOf(
                "ている" to "て", "ておる" to "て", "てる" to "て", "とる" to "て",
                "でいる" to "で", "でおる" to "で", "でる" to "で", "どる" to "で"
            )) {
                rule("progressive or perfect", from, to, v1, te)
            }

            // ---- Auxiliaries on the te-form ----
            for ((from, to) in listOf("てしまう" to "て", "でしまう" to "で")) {
                rule("-shimau", from, to, v5, te)
            }
            for ((from, to) in listOf("ておく" to "て", "でおく" to "で")) {
                rule("-oku", from, to, v5, te)
            }

            // ---- Contractions of てしまう and ておく ----
            for ((name, aux) in listOf("-chau" to "ちゃう", "-chimau" to "ちまう")) {
                val voiced = if (aux == "ちゃう") "じゃう" else "じまう"
                rule(name, aux, "る", v5, v1)
                godanEuphonic(name, { it.dropLast(1) + if (it.endsWith("で")) voiced else aux }, v5)
                irregular(name, v5, "し$aux", "き$aux")
            }
            rule("-toku", "とく", "る", v5, v1)
            godanEuphonic("-toku", { it.dropLast(1) + if (it.endsWith("で")) "どく" else "とく" }, v5)
            irregular("-toku", v5, "しとく", "きとく")

            // ---- Polite ----
            for ((name, suffix) in listOf(
                "polite" to "ます",
                "polite negative" to "ません",
                "polite past" to "ました",
                "polite past negative" to "ませんでした",
                "polite volitional" to "ましょう"
            )) {
                rule(name, suffix, "る", 0, v1)
                godan(name, I, suffix, 0)
                irregular(name, 0, "し$suffix", "き$suffix")
            }
            rule("polite negative", "くありません", "い", 0, adj)
            rule("polite past negative", "くありませんでした", "い", 0, adj)

            // ---- Desire: たい behaves as an i-adjective ----
            rule("-tai", "たい", "る", adj, v1)
            godan("-tai", I, "たい", adj)
            irregular("-tai", adj, "したい", "きたい")

            // ---- Volitional ----
            rule("volitional", "よう", "る", 0, v1)
            for (i in U.indices) rule("volitional", O[i] + "う", U[i].toString(), 0, v5)
            irregular("volitional", 0, "しよう", "こよう")
            rule("volitional", "かろう", "い", 0, adj)

            // ---- Imperative ----
            rule("imperative", "ろ", "る", 0, v1)
            rule("imperative", "よ", "る", 0, v1)
            for (i in U.indices) rule("imperative", E[i].toString(), U[i].toString(), 0, v5)
            rule("imperative", "しろ", "する", 0, vs)
            rule("imperative", "せよ", "する", 0, vs)
            rule("imperative", "こい", "くる", 0, vk)
            rule("imperative", "来い", "来る", 0, vk)
            rule("imperative negative", "な", "", 0, WordClass.ANY_VERB)

            // ---- Request: なさい and its short form な ----
            rule("-nasai", "なさい", "る", 0, v1)
            godan("-nasai", I, "なさい", 0)
            irregular("-nasai", 0, "しなさい", "きなさい")

            // ---- Potential, passive, causative: all behave as ichidan verbs ----
            rule("potential or passive", "られる", "る", v1, v1)
            rule("potential", "れる", "る", v1, v1) // colloquial 食べれる
            for (i in U.indices) rule("potential", E[i] + "る", U[i].toString(), v1, v5)
            godan("passive", A, "れる", v1)
            rule("passive", "される", "する", v1, vs)
            rule("potential or passive", "こられる", "くる", v1, vk)
            rule("potential or passive", "来られる", "来る", v1, vk)
            rule("potential", "これる", "くる", v1, vk)
            rule("potential", "来れる", "来る", v1, vk)
            rule("causative", "させる", "る", v1, v1)
            godan("causative", A, "せる", v1)
            rule("causative", "させる", "する", v1, vs)
            rule("causative", "こさせる", "くる", v1, vk)
            rule("causative", "来させる", "来る", v1, vk)
            godan("causative", A, "す", v5) // short causative 読ます
            godan("causative passive", A, "される", v1)
            rule("causative passive", "させられる", "る", v1, v1)
            rule("causative passive", "させられる", "する", v1, vs)

            // ---- Conditionals ----
            rule("-ba", "れば", "る", 0, v1)
            for (i in U.indices) rule("-ba", E[i] + "ば", U[i].toString(), 0, v5)
            rule("-ba", "すれば", "する", 0, vs)
            rule("-ba", "くれば", "くる", 0, vk)
            rule("-ba", "来れば", "来る", 0, vk)
            rule("-ba", "ければ", "い", 0, adj)
            rule("-ba", "きゃ", "い", 0, adj) // なきゃ, colloquial ければ
            rule("-ba", "けりゃ", "い", 0, adj)
            rule("-tara", "たら", "る", 0, v1)
            godanEuphonic("-tara", { it.dropLast(1) + if (it.endsWith("で")) "だら" else "たら" }, 0)
            irregular("-tara", 0, "したら", "きたら")
            rule("-tara", "かったら", "い", 0, adj)
            rule("-tari", "たり", "る", 0, v1)
            godanEuphonic("-tari", { it.dropLast(1) + if (it.endsWith("で")) "だり" else "たり" }, 0)
            irregular("-tari", 0, "したり", "きたり")
            rule("-tari", "かったり", "い", 0, adj)

            // ---- ては contractions ----
            rule("-cha", "ちゃ", "る", 0, v1)
            godanEuphonic("-cha", { it.dropLast(1) + if (it.endsWith("で")) "じゃ" else "ちゃ" }, 0)
            irregular("-cha", 0, "しちゃ", "きちゃ")
            rule("-cha", "くちゃ", "い", 0, adj)

            // ---- Classical and colloquial negatives ----
            for ((name, suffix) in listOf("-zu" to "ず", "-nu" to "ぬ", "-n" to "ん")) {
                rule(name, suffix, "る", 0, v1)
                godan(name, A, suffix, 0)
                rule(name, "せ$suffix", "する", 0, vs)
                rule(name, "こ$suffix", "くる", 0, vk)
                rule(name, "来$suffix", "来る", 0, vk)
            }
            for (slang in listOf("ねえ", "ねぇ", "ねー")) rule("slang", slang, "ない", 0, adj)
            for (slang in listOf("てえ", "てぇ", "てー")) rule("slang", slang, "たい", 0, adj)

            // ---- Appearance and excess ----
            rule("-sou", "そう", "る", 0, v1)
            godan("-sou", I, "そう", 0)
            irregular("-sou", 0, "しそう", "きそう")
            rule("-sou", "そう", "い", 0, adj)
            for (sugiru in listOf("すぎる", "過ぎる")) {
                rule("-sugiru", sugiru, "る", v1, v1)
                godan("-sugiru", I, sugiru, v1)
                irregular("-sugiru", v1, "し$sugiru", "き$sugiru")
                rule("-sugiru", sugiru, "い", v1, adj)
            }

            // ---- Stems and adjective derivations ----
            rule("masu stem", "", "る", 0, v1)
            for (i in U.indices) rule("masu stem", I[i].toString(), U[i].toString(), 0, v5)
            rule("masu stem", "し", "する", 0, vs)
            rule("masu stem", "き", "くる", 0, vk)
            rule("masu stem", "来", "来る", 0, vk)
            rule("adverb", "く", "い", 0, adj)
            rule("noun", "さ", "い", 0, adj)
            rule("-ki", "き", "い", 0, adj)
            rule("-ge", "げ", "い", 0, adj)
            rule("-garu", "がる", "い", v5, adj)

            return rules
        }
    }
}
