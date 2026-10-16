#!/usr/bin/env python3
"""Writes tests/fixtures/normalisation_core_cases.tsv: the CORE module's own normalisation cases (DESIGN.md section 4).

Expected values come from a reference implementation of the section 4 rules on top of Python's unicodedata (full
NFC/NFD, str.lower), so the C++ table-driven code is checked against the real Unicode algorithms. The shared golden
file written by the library pipeline is tests/fixtures/normalisation_golden.tsv; this file is the CORE subset.
Columns: function, input, expected. Inputs never contain tabs or newlines.
Usage: python3 engine/core/tools/gen_core_cases.py [output]
"""
import os
import sys
import unicodedata as ud

LENGTH = {'\u0304', '\u0306'}
GREEK_MARKS = {'\u0300', '\u0301', '\u0302', '\u0308', '\u0313', '\u0314', '\u0342', '\u0343', '\u0344', '\u0345'}
QUOTES = {'\u2018': "'", '\u2019': "'", '\u201a': "'", '\u201b': "'",
          '\u201c': '"', '\u201d': '"', '\u201e': '"', '\u201f': '"'}


def nfc(s): return ud.normalize('NFC', s)
def nfd(s): return ud.normalize('NFD', s)
def strip(s, marks): return nfc(''.join(c for c in nfd(s) if c not in marks))


def latin_key(s):
    t = strip(nfc(s).lower(), LENGTH)
    t = t.replace('j', 'i').replace('v', 'u').replace('\u00e6', 'ae').replace('\u0153', 'oe')
    return ''.join(c for c in t if c.isalpha() or c in '- ')


def greek_key(s): return strip(nfc(s).lower().replace('\u03c2', '\u03c3'), LENGTH)
def greek_bare(s): return strip(greek_key(s), GREEK_MARKS)
def en_key(s): return ''.join(QUOTES.get(c, c) for c in nfc(s).lower())
def es_key(s): return en_key(s)
def es_bare(s): return nfc(''.join(c for c in nfd(es_key(s)) if not ('\u0300' <= c <= '\u036f')))
def display_latin(s): return strip(s, LENGTH)   # macrons=false (macrons=true is nfc)
def lower(s): return s.lower()


FUNCS = {'nfc': nfc, 'nfd': nfd, 'lower': lower, 'latin_key': latin_key, 'greek_key': greek_key,
         'greek_bare': greek_bare, 'en_key': en_key, 'es_key': es_key, 'es_bare': es_bare,
         'display_latin': display_latin}

M = '\u0304'  # combining macron
B = '\u0306'  # combining breve
CASES = {
    'nfc': ['a' + M, 'A' + M + '\u0301', 'e' + B, 'Iu' + M + 'lius', 'o' + '\u0308' + M, 'u' + '\u0308' + M,
            'n\u0303o', 'a\u0323\u0302', 'a\u0302\u0323', 'e\u0327\u0306', '\u03b1\u0313\u0301\u0345',
            '\u03b1\u0345\u0313\u0301', '\u03c9\u0314\u0342\u0345', '\u0391\u0313', '\u1f71', '\u037e', '\u0387',
            '\u03b9\u0308\u0301', 'q\u0301', '\u0301a', 'A\u030a', '\u017f\u0307', 'e\u0304\u0301',
            '\u0292\u030c', 'plain ascii', '\u0434\u0306', '\u1fbe', '\u1ff3\u0301'],
    'nfd': ['\u0101', '\u1e17', '\u1f84', '\u00e9', '\u01d6', '\u1fb3', '\u0385', 'a\u0345\u0301', 'abc',
            '\u1e69', '\u01fd', '\u1f00\u0301'],
    'lower': ['IVLIVS', '\u0100', '\u0130', '\u039f\u0394\u039f\u03a3', '\u03a3\u039f\u03a6\u0399\u0391',
              '\u039b\u039f\u0393\u039f\u03a3 \u039a\u0391\u0399', '\u1f88', '\u1fbc', '\u00c6SOP', '\u0152',
              '\u1e9e', 'MiXeD', '\u03a3', 'A\u03a3', 'A\u03a3.', '\u0410\u0411', '\u0391\u0301\u03a3'],
    'latin_key': ['Iu' + M + 'lius', '\u012auli\u0101', 'vir', 'S\u012bc', 'Jupiter', '\u0100ENE\u0100S',
                  'C\u00e6sar', '\u0152dipus', 'r\u014dsa', 'ro' + M + 'sa', 'r\u014f' + 'sa', 'gr\u0103tus',
                  '\u0233', 'ad-uenio', 'non est', 'ita, vero!', 'equus2', 'mih\u012b', 'tib\u012d',
                  'iam', 'VVLGVS', 'j\u016bs', 'e\u0304\u0301', 'qu\u014d modo', 'caf\u00e9', '\u00e6quus',
                  'Pr\u00e6t\u014dr', 'ma\u0304lu\u0306m', '\u1e17', '\u01d6', 'x\u0301', 'Ianu\u0101rius',
                  'Iul.', '\u00ab\u0101\u00bb'],
    'greek_key': ['\u03bb\u03cc\u03b3\u03bf\u03c2', '\u039b\u038c\u0393\u039f\u03a3', '\u1f00\u03bd\u03ae\u03c1',
                  '\u1f08\u039d\u0389\u03a1', '\u03b1' + M, '\u1fb1', '\u1fb0', '\u1fbc', '\u1f84',
                  '\u03b8\u03b5\u03cc\u03c2 \u1f10\u03c3\u03c4\u03af', '\u1f71\u03bd\u03b8\u03c1\u03c9\u03c0\u03bf\u03c2',
                  '\u03c3\u03bf\u03c6\u03af\u03b1\u03c2', '\u1f40\u03b4\u03cc\u03c2', '\u1fe5\u03ae\u03c4\u03c9\u03c1',
                  '\u03b9' + B, '\u1fd1', '\u0391\u03b8\u1fc6\u03bd\u03b1\u03b9'],
    'greek_bare': ['\u03bb\u03cc\u03b3\u03bf\u03c2', '\u1f00\u03bd\u03ae\u03c1', '\u1f84', '\u1ff7',
                   '\u1f41\u03b4\u03cc\u03c2', '\u1fe5\u03ae\u03c4\u03c9\u03c1', '\u03ca', '\u0390', '\u1fd3',
                   '\u1f0c\u03a1\u0397\u03a3', '\u1fb7', '\u03c8\u03c5\u03c7\u1fc7', '\u1f22', '\u1fe5',
                   '\u1fbd', '\u1f67', '\u0391\u1f50\u03c4\u03cc\u03c2', '\u03b1\u0313\u0300\u0345'],
    'en_key': ['Hello', '\u201cHi\u201d', 'don\u2019t', '\u2018quoted\u2019', 'CAF\u00c9', 'cafe\u0301',
               '\u201eLow\u201f', 'ALREADY lower', 'na\u00efve', '\u0130stanbul'],
    'es_key': ['\u00bfQu\u00e9?', 'NI\u00d1O', 'nin\u0303o', '\u201cHola\u201d', 'Coraz\u00f3n', 'ping\u00fcino',
               '\u00c1rbol'],
    'es_bare': ['ni\u00f1o', 'NI\u00d1O', 'Coraz\u00f3n', 'ping\u00fcino', '\u00bfQu\u00e9?', 'acci\u00f3n',
                '\u201ceso\u201d', 'a\u0300\u0301', 'Espa\u00f1a'],
    'display_latin': ['Iu' + M + 'lius', '\u012aULI\u0100', 'r\u014dsa', 'ma\u0304lu\u0306m', 'C\u00e6sar',
                      'J\u016bn\u014d', '\u0233', '\u1e17', 'plain', '\u014f'],
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tests', 'fixtures',
        'normalisation_core_cases.tsv')
    rows = []
    for fn, inputs in CASES.items():
        for s in inputs:
            assert '\t' not in s and '\n' not in s
            rows.append((fn, s, FUNCS[fn](s)))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Generated by engine/core/tools/gen_core_cases.py (reference: Python unicodedata %s). Do not edit.\n'
                % ud.unidata_version)
        f.write('function\tinput\texpected\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    print('wrote %d rows to %s' % (len(rows), os.path.normpath(out)))


if __name__ == '__main__':
    main()
