#!/usr/bin/env python3
"""Assembles the portable Windows folder dist/vetus-poeta/ from a finished build.

    python3 tools/make_dist.py --mingw build-mingw-release          # cross build from Linux
    python tools\\make_dist.py --msvc build --config Release          # Visual Studio build
    python3 tools/make_dist.py --mingw build-mingw-release --no-data # leave the lexicons out

Layout (docs/BUILD.md "Windows: the dist folder"):
    VetusPoeta.exe        the shell (gui/shell)
    vpengine.exe          the engine sidecar (engine/cli)
    WebView2Loader.dll    MinGW builds only (MSVC links the static loader)
    ui/ + ui.manifest.json   gui/ui without dev/ and tests/ (tools/pack_ui.py), with SHA-256 per file
    data/*.vpl            lexicons copied from data/work/ when present (the shell passes --lexicons data)
    data/nlp/*.vpt        English/Spanish analysis models from data/work/nlp/ (the engine finds <lexicons>/nlp)
    data/curated/*.tsv *.txt  the teacher-editable rule tables from data/curated/ (<lexicons>/curated)
    samples/sample.<lang>.srt  the 12-cue sample files from tests/samples/ (our own sentences); the engine copies
                          them to <user data>/samples/ where the UI opens them (engine.hello.samples)
    models/README.txt     where the optional local model file goes
    licenses/             WebView2 SDK licence (when the loader DLL ships)
    THIRD_PARTY_NOTICES.txt  third_party/LICENSES.md, licence texts, and the NOTE section of every .vpl
The output folder is replaced only if it is empty or was made by this script (it has ui.manifest.json).
Prints the size of every file. Python 3 stdlib only.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_ui  # noqa: E402

MINGW_RUNTIME_DLLS = ('libstdc++', 'libgcc_s', 'libwinpthread')
MIT_TEXT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.
"""
MIT_COMPONENTS = [
    ('nlohmann/json 3.12.0', 'Copyright (c) 2013-2025 Niels Lohmann'),
    ('miniz 3.0.2', 'Copyright 2013-2014 RAD Game Tools and Valve Software\n'
                    'Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC'),
]
MODELS_README = """vetus poeta - optional local model
==================================

The rule engine works without any model. To use the optional local model (engine ii), put its GGUF
file in this folder. The file name, size and SHA-256 that this version expects are listed in
docs/BUILD.md ("The local model file"); the app checks the SHA-256 and shows the result in
Settings > Engines. The model is loaded only while a translation job runs.
"""


def read_build_facts(build):
    facts = {}
    path = os.path.join(build, 'vp_shell_build.txt')
    if os.path.isfile(path):
        for line in open(path, encoding='utf-8'):
            if '=' in line:
                k, v = line.rstrip('\n').split('=', 1)
                facts[k] = v
    return facts


def find_file(candidates):
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def vpl_note(path):
    """The NOTE section of a .vpl (DESIGN 5) as text, or None."""
    with open(path, 'rb') as f:
        head = f.read(256)
        if len(head) < 256 or head[:4] != b'VPLX':
            return None
        count = struct.unpack_from('<I', head, 16)[0]
        if count > 64:
            return None
        table = f.read(24 * count)
        for i in range(count):
            tag, _res, off, length = struct.unpack_from('<4sIQQ', table, 24 * i)
            if tag == b'NOTE' and length < (4 << 20):
                f.seek(off)
                return f.read(length).decode('utf-8', 'replace').rstrip('\0').strip()
    return None


def notices(vpls, webview2_license):
    out = []
    out.append('vetus poeta - third-party notices')
    out.append('=' * 33)
    out.append('')
    out.append('vetus poeta is proprietary software. It includes or ships with the following works by others.')
    out.append('Generated by tools/make_dist.py from third_party/LICENSES.md, the licence texts below, and the')
    out.append('NOTE section of every lexicon file in data/.')
    out.append('')
    out.append('-' * 100)
    out.append('third_party/LICENSES.md')
    out.append('-' * 100)
    out.append(open(os.path.join(REPO, 'third_party', 'LICENSES.md'), encoding='utf-8').read().strip())
    out.append('')
    out.append('-' * 100)
    out.append('MIT License (applies to each component listed)')
    out.append('-' * 100)
    for name, copyright_line in MIT_COMPONENTS:
        out.append('%s\n%s\n' % (name, copyright_line))
    out.append(MIT_TEXT)
    texts = [('llama.cpp / ggml (MIT)', os.path.join(REPO, 'third_party', 'llama', 'LICENSE')),
             ('Gentium Plus fonts (SIL Open Font License 1.1)', os.path.join(REPO, 'gui', 'ui', 'fonts', 'OFL.txt'))]
    if webview2_license:
        texts.append(('Microsoft Edge WebView2 SDK (WebView2Loader)', webview2_license))
    for title, path in texts:
        if os.path.isfile(path):
            out.append('-' * 100)
            out.append(title)
            out.append('-' * 100)
            out.append(open(path, encoding='utf-8', errors='replace').read().strip())
            out.append('')
    for vpl in vpls:
        note = vpl_note(vpl)
        out.append('-' * 100)
        out.append('Lexicon %s (NOTE section)' % os.path.basename(vpl))
        out.append('-' * 100)
        out.append(note if note else '(no NOTE section found)')
        out.append('')
    return '\r\n'.join('\n'.join(out).split('\n')) + '\r\n'


def imports(exe):
    tool = shutil.which('x86_64-w64-mingw32-objdump') or shutil.which('objdump')
    if not tool:
        return None
    try:
        text = subprocess.run([tool, '-p', exe], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return sorted({line.split()[-1] for line in text.splitlines() if 'DLL Name:' in line})


def main():
    ap = argparse.ArgumentParser(description='Assemble dist/vetus-poeta from a Windows build.')
    kind = ap.add_mutually_exclusive_group(required=True)
    kind.add_argument('--mingw', metavar='BUILD', help='MinGW-w64 build folder (e.g. build-mingw-release)')
    kind.add_argument('--msvc', metavar='BUILD', help='Visual Studio build folder (multi-config)')
    ap.add_argument('--config', default='Release', help='MSVC configuration (default Release)')
    ap.add_argument('--out', default=os.path.join(REPO, 'dist', 'vetus-poeta'))
    ap.add_argument('--no-data', action='store_true', help='do not copy data/work/*.vpl')
    args = ap.parse_args()

    build = os.path.abspath(args.mingw or args.msvc)
    cfg = [] if args.mingw else [args.config]
    shell_exe = find_file([os.path.join(build, 'gui', 'shell', *cfg, 'VetusPoeta.exe'),
                           os.path.join(build, 'app', *cfg, 'VetusPoeta.exe')])
    engine_exe = find_file([os.path.join(build, 'engine', 'cli', *cfg, 'vpengine.exe'),
                            os.path.join(build, 'app', *cfg, 'vpengine.exe')])
    if not shell_exe or not engine_exe:
        sys.exit('make_dist: VetusPoeta.exe or vpengine.exe not found under %s (build with -DVP_BUILD_GUI=ON)' % build)
    facts = read_build_facts(build)
    loader = facts.get('webview2_loader_dll', '')
    static = facts.get('webview2_static', '0') == '1'

    out = os.path.abspath(args.out)
    if os.path.isdir(out) and os.listdir(out):
        if not os.path.isfile(os.path.join(out, pack_ui.MANIFEST_NAME)):
            sys.exit('make_dist: %s is not empty and was not made by make_dist.py; refusing to replace it' % out)
        shutil.rmtree(out)
    os.makedirs(out)

    shutil.copy2(shell_exe, out)
    shutil.copy2(engine_exe, out)
    warnings = []
    licence = facts.get('webview2_license', '')
    if not static:
        if loader and os.path.isfile(loader):
            shutil.copy2(loader, out)
        else:
            warnings.append('WebView2Loader.dll not found (offline configure?): copy an x64 one next to '
                            'VetusPoeta.exe or the app will not start')
    if licence and os.path.isfile(licence):
        os.makedirs(os.path.join(out, 'licenses'))
        shutil.copyfile(licence, os.path.join(out, 'licenses', 'WebView2-LICENSE.txt'))

    pack_ui.pack(os.path.join(out, 'ui'), quiet=True)

    vpls = []
    work = os.path.join(REPO, 'data', 'work')
    if not args.no_data and os.path.isdir(work):
        names = sorted(n for n in os.listdir(work) if n.endswith('.vpl'))
        if names:
            os.makedirs(os.path.join(out, 'data'))
        for n in names:
            dst = os.path.join(out, 'data', n)
            shutil.copyfile(os.path.join(work, n), dst)
            vpls.append(dst)
    if not vpls:
        warnings.append('no lexicons copied (data/work/*.vpl missing or --no-data): the engine reports lexicon_missing')
    # NLP models and curated tables next to the lexicons (engine/cli/README.md "Dist layout")
    if not args.no_data:
        nlp = os.path.join(work, 'nlp')
        models = sorted(n for n in os.listdir(nlp) if n.endswith('.vpt')) if os.path.isdir(nlp) else []
        if models:
            os.makedirs(os.path.join(out, 'data', 'nlp'), exist_ok=True)
            for n in models:
                shutil.copyfile(os.path.join(nlp, n), os.path.join(out, 'data', 'nlp', n))
        if len(models) < 4:
            warnings.append('data/work/nlp/*.vpt incomplete (%d of 4): English/Spanish sources are unavailable' % len(models))
        curated = os.path.join(REPO, 'data', 'curated')
        tables = sorted(n for n in os.listdir(curated) if n.endswith(('.tsv', '.txt'))) if os.path.isdir(curated) else []
        os.makedirs(os.path.join(out, 'data', 'curated'), exist_ok=True)
        for n in tables:
            shutil.copyfile(os.path.join(curated, n), os.path.join(out, 'data', 'curated', n))
        if 'order_la.txt' not in tables:
            warnings.append('data/curated/order_la.txt missing: the rule engine cannot start')
        # the pairs engine.hello offers depend on these (engine/cli/README.md "engine.hello")
        if vpls and 'greek.vpl' not in [os.path.basename(v) for v in vpls]:
            warnings.append('greek.vpl not copied: en-grc, es-grc, grc-en, grc-es are unavailable (lexicon_missing)')
        if 'simplify_la.tsv' not in tables:
            warnings.append('data/curated/simplify_la.tsv missing: Orbergise (la-la) is unavailable')
        if 'order_grc.txt' not in tables:
            warnings.append('data/curated/order_grc.txt missing: the Greek pairs are unavailable')
    samples = os.path.join(REPO, 'tests', 'samples')
    srts = sorted(n for n in os.listdir(samples) if n.startswith('sample.') and n.endswith('.srt')) if os.path.isdir(samples) else []
    if srts:
        os.makedirs(os.path.join(out, 'samples'))
        for n in srts:
            shutil.copyfile(os.path.join(samples, n), os.path.join(out, 'samples', n))
    else:
        warnings.append('tests/samples/sample.*.srt missing: "Try the sample" has no file')

    os.makedirs(os.path.join(out, 'models'))
    with open(os.path.join(out, 'models', 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(MODELS_README)
    with open(os.path.join(out, 'THIRD_PARTY_NOTICES.txt'), 'w', encoding='utf-8', newline='') as f:
        f.write(notices(vpls, licence if os.path.isfile(licence or '') else ''))

    total = 0
    rows = []
    for root, _dirs, files in os.walk(out):
        for n in files:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, out).replace(os.sep, '/')
            size = os.path.getsize(p)
            total += size
            if not rel.startswith('ui/'):
                rows.append((rel, size))
    ui_bytes = sum(os.path.getsize(os.path.join(r, n)) for r, _d, fs in os.walk(os.path.join(out, 'ui')) for n in fs)
    rows.append(('ui/ (%d files)' % sum(len(fs) for _r, _d, fs in os.walk(os.path.join(out, 'ui'))), ui_bytes))
    for rel, size in sorted(rows):
        print('%14s  %s' % ('{:,}'.format(size), rel))
    print('%14s  total in %s' % ('{:,}'.format(total), out))

    failed = False
    for exe in (os.path.join(out, 'VetusPoeta.exe'), os.path.join(out, 'vpengine.exe')):
        dlls = imports(exe)
        if dlls is None:
            continue
        print('  %s imports: %s' % (os.path.basename(exe), ' '.join(dlls)))
        bad = [d for d in dlls if d.lower().startswith(MINGW_RUNTIME_DLLS)]
        if bad:
            print('make_dist: %s depends on MinGW runtime DLLs %s' % (os.path.basename(exe), bad), file=sys.stderr)
            failed = True
    for w in warnings:
        print('make_dist: WARNING ' + w, file=sys.stderr)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
