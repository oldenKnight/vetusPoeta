"""Whitaker-only analyses for Latin (B4b; ANAL flag bit2, DESIGN 5).

Whitaker's Words accepts a form when a DICTLINE stem plus an INFLECTS ending of a matching paradigm spells it. This
module runs that generator over the DICTLINE entries that import_aux joined to a Kaikki lemma (whitaker.tsv,
lemma_ids column) and returns the forms Kaikki does not have at all (no analysis of that key for any lemma), as ANAL
rows with flag bit2: a Whitaker-only reading never adds ambiguity to a word Kaikki knows (Whitaker's 1st-declension
genitive plural in -um would otherwise make "magistrum" a genitive of magistra and hide a case-government error).

Scope (what is generated):
  N   declensions 1-5: case + number (noun table cells carry no gender); inflection gender X matches any entry,
      C matches M/F/C, an entry of gender C takes M and F endings
  ADJ declensions 1-3, positive degree only (inflections POS or X): case, gender (C -> masculine+feminine, X -> all
      three), number
  V   conjugations 1-4 (+ the "0 0" perfect-system endings shared by all): tense, voice, mood, person, number;
      deponents (DEP) use the passive endings as active forms (Kaikki tags deponent forms "active"), semi-deponents
      the active present system only, impersonals the 3rd person singular and the infinitives only, PERFDEF the
      perfect system only; endings of length 0 are skipped for verbs (Whitaker's "dic / duc / fac" imperatives)
  Not generated: participles (VPAR: their declined forms belong to the participle lemmas), supines, comparatives,
  pronouns, numerals, irregular verbs (conjugations 5-9: Kaikki's tables cover them), PACK lines.
Inflection lines are used when their age is X, B or C and their frequency A, B or C. Flags: bit2 always; bit6
(poetic/rare) when the ending is not Whitaker's first choice (frequency B or C) or early (age B), or the entry is
archaic (age A/B); bit5 when the entry is late or medieval (age D-H).
Guards: (1) when the lemma has an inflection table and fewer than half of the forms an entry generates are already
known for it, the entry is not used for that lemma (a wrong join; counted as `entry_rejected`); (2) for a lemma with
a table a Whitaker form is added only for a feature word the table already fills (Whitaker supplies other spellings
of existing cells, -um/-ium, -ere/-ērunt, -re/-ris; it never adds a passive to an intransitive verb or a plural to
a singular-only noun: `feature_not_in_table`).
Display: stem + ending as Whitaker spells it, j -> i (Orberg, D13); no length marks (Whitaker has none).
"""
import collections
import os

import lexdata
import tagmap
import vptext

CASES = {"NOM": "nominative", "GEN": "genitive", "DAT": "dative", "ACC": "accusative", "ABL": "ablative",
         "VOC": "vocative", "LOC": "locative"}
NUMS = {"S": "singular", "P": "plural"}
GENDERS = {"M": ["masculine"], "F": ["feminine"], "N": ["neuter"], "C": ["masculine", "feminine"],
           "X": ["masculine", "feminine", "neuter"]}
TENSES = {"PRES": "present", "IMPF": "imperfect", "FUT": "future", "PERF": "perfect", "PLUP": "pluperfect",
          "FUTP": "future-perfect"}
PERFECT_SYSTEM = frozenset(("PERF", "PLUP", "FUTP"))
MOODS = {"IND": "indicative", "SUB": "subjunctive", "IMP": "imperative", "INF": "infinitive"}
PERSONS = {"1": "first-person", "2": "second-person", "3": "third-person"}
USE_AGES = frozenset("XBC")
USE_FREQS = frozenset("ABC")
FLAG_WHITAKER, FLAG_LATE, FLAG_RARE = 4, 32, 64
POS_FOR = {"N": ("noun", "name"), "ADJ": ("adj",), "V": ("verb",)}


class Inflection(object):
    __slots__ = ("pos", "d", "v", "fields", "stem", "ending", "age", "freq")

    def __init__(self, row):
        self.pos = row[0]
        g = row[1].split()
        self.d, self.v, self.fields = g[0], g[1], g[2:]
        self.stem = int(row[2]) if row[2].isdigit() else 0
        self.ending = row[4] if len(row) > 4 else ""
        self.age, self.freq = (row[5] if len(row) > 5 else "X"), (row[6] if len(row) > 6 else "X")


def load_inflections(path):
    """INFLECTS rows (whitaker_inflects.tsv) usable here, grouped by POS."""
    out = collections.defaultdict(list)
    for r in lexdata.read_rows(path):
        if len(r) < 7 or r[0] not in POS_FOR:
            continue
        inf = Inflection(r)
        if inf.age not in USE_AGES or inf.freq not in USE_FREQS or not inf.stem:
            continue
        if inf.pos == "V" and not inf.ending:
            continue
        out[inf.pos].append(inf)
    return out


def _paradigm_ok(inf, d, v):
    if inf.d == "0":
        return True
    return inf.d == d and (inf.v == "0" or inf.v == v)


def _gender_ok(ig, eg):
    return ig == "X" or eg == "X" or ig == eg or (ig == "C" and eg in "MFC") or (eg == "C" and ig in "MF")


def entry_forms(entry, infl):
    """[(form, [tag lists], rare)] for one DICTLINE entry (dict with stems, pos, codes). An adjective ending for
    several genders gives the combined tag list first, then one list per gender."""
    pos, codes, stems = entry["pos"], entry["codes"] + ["", "", "", ""], entry["stems"]
    out = []
    if pos == "N":
        d, v, eg = codes[0], codes[1], codes[2] or "X"
        if d not in "12345" or not d:
            return out
        for inf in infl.get("N", ()):
            f = inf.fields
            if len(f) < 3 or not _paradigm_ok(inf, d, v) or f[0] not in CASES or f[1] not in NUMS:
                continue
            if not _gender_ok(f[2], eg):
                continue
            out.append((inf, [[CASES[f[0]], NUMS[f[1]]]]))
    elif pos == "ADJ":
        d, v, cmp_ = codes[0], codes[1], codes[2] or "X"
        if d not in "123" or not d or cmp_ not in ("POS", "X"):
            return out
        for inf in infl.get("ADJ", ()):
            f = inf.fields
            if len(f) < 4 or not _paradigm_ok(inf, d, v) or f[0] not in CASES or f[1] not in NUMS:
                continue
            if f[3] not in ("POS", "X") or f[2] not in GENDERS:
                continue
            base = [CASES[f[0]], NUMS[f[1]]]
            sets = [base + GENDERS[f[2]]]
            if len(GENDERS[f[2]]) > 1:
                sets += [base + [g] for g in GENDERS[f[2]]]
            out.append((inf, sets))
    elif pos == "V":
        c, v, kind = codes[0], codes[1], codes[2] or "X"
        if c not in "1234" or not c:
            return out
        for inf in infl.get("V", ()):
            f = inf.fields
            if len(f) < 5 or not _paradigm_ok(inf, c, v):
                continue
            tense, voice, mood, person, num = f[:5]
            if tense not in TENSES or mood not in MOODS or voice not in ("ACTIVE", "PASSIVE"):
                continue
            if kind == "DEP":
                if voice != "PASSIVE":
                    continue
                voice = "ACTIVE"
            elif kind == "SEMIDEP":
                if voice != "ACTIVE" or tense in PERFECT_SYSTEM:
                    continue
            elif kind == "PERFDEF" and tense not in PERFECT_SYSTEM:
                continue
            if kind == "IMPERS" and mood != "INF" and (person != "3" or num != "S"):
                continue
            tags = [TENSES[tense], voice.lower(), MOODS[mood]]
            if mood != "INF":
                if person not in PERSONS or num not in NUMS:
                    continue
                tags += [PERSONS[person], NUMS[num]]
            out.append((inf, [tags]))
    res = []
    for inf, tags in out:
        stem = stems[inf.stem - 1] if inf.stem <= len(stems) else ""
        if not stem:
            continue
        form = (stem + inf.ending).replace("j", "i").replace("J", "I")
        rare = inf.age == "B" or inf.freq != "A"
        res.append((form, tags, rare))
    return res


def generate(whitaker_rows, infl, lemma_fpos, lemma_has_table, known, c, has_cell=None, key_exists=None):
    """ANAL rows (key, lemma_id, packed, flags, display) that Whitaker accepts and Kaikki lacks.
    whitaker_rows: rows of whitaker.tsv; lemma_fpos / lemma_has_table: lists by lemma id; known(key, lid) -> bool
    (Kaikki already has an analysis of this key for this lemma); has_cell(lid, packed) -> bool (the lemma's table
    fills this feature word; None = no guard); key_exists(key) -> bool (Kaikki has an analysis of this key for any
    lemma: the form is not added; None = only the lemma's own forms are excluded); c: Counter for the report."""
    stats = tagmap.TagStats()
    rows = {}
    for r in whitaker_rows:
        if len(r) < 15:
            continue
        ids = lexdata.parse_ids(r[14])
        if not ids or r[5] not in POS_FOR:
            continue
        entry = {"stems": r[1:5], "pos": r[5], "codes": r[6].split()}
        age = r[7]
        forms = entry_forms(entry, infl)
        if not forms:
            continue
        c["entries_with_forms"] += 1
        for lid in ids:
            fpos = lemma_fpos[lid]
            if fpos not in POS_FOR[r[5]]:
                c["entry_pos_mismatch"] += 1
                continue
            keyed = [(vptext.latin_key(f), f, tags, rare) for f, tags, rare in forms]
            n_known = sum(1 for k, _, _, _ in keyed if known(k, lid))
            if lemma_has_table[lid] and 2 * n_known < len(keyed):
                c["entry_rejected"] += 1
                continue
            c["entry_used"] += 1
            for k, form, tagsets, rare in keyed:
                if not k or known(k, lid):
                    continue
                if key_exists is not None and key_exists(k):
                    c["key_known_for_another_lemma"] += 1
                    continue
                fl = FLAG_WHITAKER
                if rare or age in ("A", "B"):
                    fl |= FLAG_RARE
                if age in ("D", "E", "F", "G", "H"):
                    fl |= FLAG_LATE
                packs = []
                guard = has_cell is not None and lemma_has_table[lid]
                for tags in tagsets:
                    for packed in tagmap.tags_to_feature_list(tags, fpos, stats=stats):
                        if (not guard or has_cell(lid, packed)) and packed not in packs:
                            packs.append(packed)
                    if packs and not guard:
                        break  # no table: the combined tag list only
                if not packs:
                    c["feature_not_in_table"] += 1
                    continue
                for packed in packs:
                    sk = (k, lid, packed, form)
                    old = rows.get(sk)
                    # the same form from two entries: rare/late only when both say so
                    rows[sk] = fl if old is None else (FLAG_WHITAKER | (old & fl))
    out = [(k, lid, packed, fl, form) for (k, lid, packed, form), fl in rows.items()]
    out.sort(key=lambda a: (a[0], a[1], a[2], a[4]))
    c["analyses"] = len(out)
    c["keys"] = len(set(a[0] for a in out))
    c["lemmas"] = len(set(a[1] for a in out))
    c["lemmas_without_table"] = len(set(a[1] for a in out if not lemma_has_table[a[1]]))
    return out


def run_for_pack(out_dir, lemma_fpos, lemma_has_table, known, c, has_cell=None, key_exists=None):
    d = os.path.join(out_dir, "la")
    infl = load_inflections(os.path.join(d, "whitaker_inflects.tsv"))
    return generate(lexdata.read_rows(os.path.join(d, "whitaker.tsv")), infl, lemma_fpos, lemma_has_table, known, c,
                    has_cell, key_exists)
