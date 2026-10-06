"""Packed morphological features. [CONTRACT, DESIGN.md section 5 FEAT] Identical to engine/lex/include/vp/features.h.
Bit layout (LSB first): pos 0-4 | case 5-8 | number 9-10 | gender 11-13 | person 14-15 | tense 16-19 | mood 20-22
                        | voice 23-24 | degree 25-26 | extra 27-31
"""
POS = {"none": 0, "noun": 1, "verb": 2, "adj": 3, "adv": 4, "pron": 5, "num": 6, "prep": 7, "conj": 8, "intj": 9,
       "det": 10, "name": 11, "particle": 12, "participle": 13, "phrase": 14, "suffix": 15, "prefix": 16,
       "article": 17, "postp": 18, "symbol": 19, "punct": 20, "other": 31}
CASE = {"none": 0, "nominative": 1, "genitive": 2, "dative": 3, "accusative": 4, "ablative": 5, "vocative": 6, "locative": 7}
NUMBER = {"none": 0, "singular": 1, "plural": 2, "dual": 3}
GENDER = {"none": 0, "masculine": 1, "feminine": 2, "neuter": 3, "mf": 4, "mn": 5, "fn": 6, "mfn": 7}
PERSON = {"none": 0, "first-person": 1, "second-person": 2, "third-person": 3}
TENSE = {"none": 0, "present": 1, "imperfect": 2, "future": 3, "perfect": 4, "pluperfect": 5, "future-perfect": 6, "aorist": 7}
MOOD = {"none": 0, "indicative": 1, "subjunctive": 2, "imperative": 3, "infinitive": 4, "participle": 5, "gerund": 6, "optative": 7}
VOICE = {"none": 0, "active": 1, "passive": 2, "middle": 3}
DEGREE = {"none": 0, "positive": 1, "comparative": 2, "superlative": 3}
EXTRA = {"supine": 1, "gerundive": 2, "attic": 4, "alternative": 8, "contracted": 16}

FIELDS = (("pos", 0, 31), ("case", 5, 15), ("number", 9, 3), ("gender", 11, 7), ("person", 14, 3), ("tense", 16, 15),
          ("mood", 20, 7), ("voice", 23, 3), ("degree", 25, 3), ("extra", 27, 31))


def pack(pos=0, case=0, number=0, gender=0, person=0, tense=0, mood=0, voice=0, degree=0, extra=0):
    vals = {"pos": pos, "case": case, "number": number, "gender": gender, "person": person, "tense": tense,
            "mood": mood, "voice": voice, "degree": degree, "extra": extra}
    out = 0
    for name, shift, mask in FIELDS:
        out |= (int(vals[name]) & mask) << shift
    return out


def unpack(p):
    return {name: (p >> shift) & mask for name, shift, mask in FIELDS}
