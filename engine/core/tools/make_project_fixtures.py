#!/usr/bin/env python3
"""Writes the .vpoeta fixtures in tests/fixtures/project/ with Python's zipfile (an independent zip writer, so the
C++ reader is tested against archives miniz did not produce). DESIGN.md section 8.

  sample.srt              our own three-cue subtitle (CRLF line ends, UTF-8 BOM)
  format1_python.vpoeta   a current-format project built from sample.srt
  format0.vpoeta          the same content declared as format 0 (exercises the migration step)
  no_manifest.vpoeta      the entries without manifest.json (must be project_corrupt)
Usage: python3 engine/core/tools/make_project_fixtures.py
"""
import hashlib
import json
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'tests', 'fixtures', 'project'))

SRT = ('\ufeff1\r\n00:00:01,000 --> 00:00:03,000\r\nThe girl sees the sea.\r\n\r\n'
       '2\r\n00:00:04,000 --> 00:00:06,500\r\n<i>The boy runs home.</i>\r\n\r\n'
       '3\r\n00:00:07,000 --> 00:00:09,000\r\nJulia is happy.\r\n').encode('utf-8')

CUES = [
    {'index': 1, 'state': 'translated', 'target': 'Puella mare videt.', 'chosen': 0,
     'alternatives': [{'text': 'Puella mare videt.', 'reason': 'rules', 'score': 0.9},
                      {'text': 'Puella mare spectat.', 'reason': 'synonym', 'score': 0.6}],
     'confidence': 'ok', 'score': 0.9, 'checks': [{'id': 'A1', 'ok': True, 'detail': ''}],
     'reasons': [{'tokenIndex': 1, 'kind': 'sense', 'text': 'see -> videre', 'data': {'lemma': 'video'}}],
     'edited': False, 'reviewed': False},
    {'index': 2, 'state': 'edited', 'target': 'Puer domum currit.', 'chosen': -1, 'alternatives': [],
     'confidence': 'check', 'score': 0.5, 'checks': [], 'reasons': [], 'edited': True, 'reviewed': True},
    {'index': 3, 'state': 'new', 'target': '', 'chosen': -1, 'alternatives': [], 'confidence': '', 'score': 0,
     'checks': [], 'reasons': [], 'edited': False, 'reviewed': False},
]
GLOSSARY = [{'name': 'Julia', 'policy': 'decline', 'form': 'I\u016blia', 'forms': ['I\u016bliae', 'I\u016bliam'],
             'gender': 'f', 'declension': '1'}]
CORRECTIONS = [{'id': 'c1', 'key': 'runs home', 'target': 'domum currit', 'scope': 'phrase', 'count': 2}]
STATS = {'cues': 3, 'translated': 2}


def manifest(fmt):
    return {'format': fmt, 'appVersion': '0.0.1', 'created': '2026-10-06T08:00:00Z', 'pair': 'en-la',
            'kind': 'subs', 'sourceFileName': 'sample.srt', 'sourceSha256': hashlib.sha256(SRT).hexdigest(),
            'settings': {'defaultFidelity': 2}}


def write(name, fmt, with_manifest=True):
    path = os.path.join(OUT, name)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        def add(n, data):
            info = zipfile.ZipInfo(n, date_time=(2000, 1, 1, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
        if with_manifest:
            add('manifest.json', json.dumps(manifest(fmt), ensure_ascii=False, indent=2))
        add('source.bin', SRT)
        add('cues.jsonl', ''.join(json.dumps(c, ensure_ascii=False) + '\n' for c in CUES))
        add('glossary.json', json.dumps(GLOSSARY, ensure_ascii=False))
        add('corrections.json', json.dumps(CORRECTIONS, ensure_ascii=False))
        add('stats.json', json.dumps(STATS))


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'sample.srt'), 'wb') as f:
        f.write(SRT)
    write('format1_python.vpoeta', 1)
    write('format0.vpoeta', 0)
    write('no_manifest.vpoeta', 1, with_manifest=False)
    print('wrote fixtures to', OUT)


if __name__ == '__main__':
    main()
