#!/usr/bin/env python3
"""Copies the web UI (gui/ui) into the folder the Windows shell serves at https://app.vetuspoeta/
(VetusPoeta.exe maps <exe dir>/ui with SetVirtualHostNameToFolderMapping), and writes a manifest
with the size and SHA-256 of every file next to it (<out>/../ui.manifest.json).

    python3 tools/pack_ui.py --out dist/vetus-poeta/ui      # copy + manifest + size report
    python3 tools/pack_ui.py --out build/app/ui --quiet     # what the CMake target vp_app runs
    python3 tools/pack_ui.py --list                         # print what would be copied

Left out: gui/ui/dev/ (browser harness), gui/ui/tests/, *.md, hidden files, caches. Kept:
js/vp_mock_engine.js (index.html loads it; it stays idle because window.chrome.webview exists),
fonts/OFL.txt (the font licence travels with the fonts). The output folder is synchronised:
files that are no longer in gui/ui are removed, but only from a folder this script made before
(it refuses to touch a non-empty folder without its manifest). Budgets of PREDESIGN 6.2 (same
rules as tools/jstest): JS <= 300 KB gzip (sum per file), CSS <= 60 KB, fonts <= 2.6 MB; the
script exits 1 when one is exceeded (--no-budget reports only). Python 3 stdlib only.
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
UI_DIR = os.path.join(REPO, 'gui', 'ui')
SKIP_DIRS = {'dev', 'tests', '__pycache__', 'node_modules'}
SKIP_EXT = {'.md', '.py', '.pyc', '.map'}
MANIFEST_NAME = 'ui.manifest.json'
JS_BUDGET_GZIP = 300 * 1024
CSS_BUDGET = 60 * 1024
FONT_BUDGET = int(2.6 * 1024 * 1024)


def collect(ui_dir=UI_DIR):
    """Sorted list of (relative posix path, absolute path)."""
    items = []
    for root, dirs, files in os.walk(ui_dir):
        rel_root = os.path.relpath(root, ui_dir)
        if rel_root == '.':
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith('.'))
        else:
            dirs[:] = sorted(d for d in dirs if d not in ('__pycache__', 'node_modules') and not d.startswith('.'))
        for name in files:
            if name.startswith('.') or os.path.splitext(name)[1].lower() in SKIP_EXT:
                continue
            full = os.path.join(root, name)
            items.append((os.path.relpath(full, ui_dir).replace(os.sep, '/'), full))
    items.sort()
    return items


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def budgets(items):
    js_gz = css = fonts = 0
    for rel, full in items:
        if rel.startswith('js/') and rel.endswith('.js'):
            with open(full, 'rb') as f:
                js_gz += len(gzip.compress(f.read(), 9, mtime=0))
        elif rel.startswith('css/') and rel.endswith('.css'):
            css += os.path.getsize(full)
        elif rel.startswith('fonts/') and rel.endswith('.ttf'):
            fonts += os.path.getsize(full)
    problems = []
    if js_gz > JS_BUDGET_GZIP:
        problems.append('JS is %d bytes gzip, budget %d' % (js_gz, JS_BUDGET_GZIP))
    if css > CSS_BUDGET:
        problems.append('CSS is %d bytes, budget %d' % (css, CSS_BUDGET))
    if fonts > FONT_BUDGET:
        problems.append('fonts are %d bytes, budget %d' % (fonts, FONT_BUDGET))
    report = 'JS %.1f KB gzip of 300 KB, CSS %.1f KB of 60 KB, fonts %.2f MB of 2.6 MB' % (
        js_gz / 1024, css / 1024, fonts / 1048576)
    return report, problems


def manifest_for(items):
    files = [{'path': rel, 'bytes': os.path.getsize(full), 'sha256': sha256_file(full)} for rel, full in items]
    listing = ''.join('%s %d %s\n' % (f['path'], f['bytes'], f['sha256']) for f in files)
    return {
        'generatedBy': 'tools/pack_ui.py',
        'origin': 'https://app.vetuspoeta/',
        'files': files,
        'fileCount': len(files),
        'totalBytes': sum(f['bytes'] for f in files),
        'sha256': hashlib.sha256(listing.encode('utf-8')).hexdigest(),
    }


def pack(out_dir, quiet=False, enforce_budget=True, ui_dir=UI_DIR):
    """Copies the UI into out_dir; returns the manifest dict. Raises SystemExit on refusal."""
    out_dir = os.path.abspath(out_dir)
    items = collect(ui_dir)
    if not any(rel == 'index.html' for rel, _ in items):
        raise SystemExit('pack_ui: %s/index.html not found' % ui_dir)
    report, problems = budgets(items)
    manifest_path = os.path.join(os.path.dirname(out_dir), MANIFEST_NAME)
    if os.path.isdir(out_dir) and os.listdir(out_dir) and not os.path.isfile(manifest_path):
        raise SystemExit('pack_ui: %s is not empty and has no %s next to it; refusing to overwrite' %
                         (out_dir, MANIFEST_NAME))
    os.makedirs(out_dir, exist_ok=True)
    wanted = set()
    copied = 0
    for rel, full in items:
        dst = os.path.join(out_dir, *rel.split('/'))
        wanted.add(os.path.normcase(os.path.abspath(dst)))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(full) and
                sha256_file(dst) == sha256_file(full)):
            shutil.copyfile(full, dst)
            copied += 1
    removed = 0
    for root, dirs, files in os.walk(out_dir, topdown=False):
        for name in files:
            p = os.path.join(root, name)
            if os.path.normcase(os.path.abspath(p)) not in wanted:
                os.remove(p)
                removed += 1
        for d in dirs:
            p = os.path.join(root, d)
            if not os.listdir(p):
                os.rmdir(p)
    manifest = manifest_for(items)
    text = json.dumps(manifest, indent=1, sort_keys=True) + '\n'
    old = open(manifest_path, encoding='utf-8').read() if os.path.isfile(manifest_path) else None
    if old != text:
        with open(manifest_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    if not quiet:
        print('pack_ui: %d files, %.1f KB -> %s (%d copied, %d removed)' % (
            manifest['fileCount'], manifest['totalBytes'] / 1024, out_dir, copied, removed))
        print('pack_ui: ' + report)
    for p in problems:
        print('pack_ui: BUDGET ' + p, file=sys.stderr)
    if problems and enforce_budget:
        raise SystemExit(1)
    return manifest


def main():
    ap = argparse.ArgumentParser(description='Copy gui/ui into the shell layout and write ui.manifest.json.')
    ap.add_argument('--out', help='destination ui folder (e.g. dist/vetus-poeta/ui)')
    ap.add_argument('--quiet', action='store_true', help='print only budget problems')
    ap.add_argument('--no-budget', action='store_true', help='report budget problems without failing')
    ap.add_argument('--list', action='store_true', help='list the files that would be copied and exit')
    args = ap.parse_args()
    if args.list:
        for rel, full in collect():
            print('%9d  %s' % (os.path.getsize(full), rel))
        print(budgets(collect())[0])
        return 0
    if not args.out:
        ap.error('--out is required (or --list)')
    pack(args.out, quiet=args.quiet, enforce_budget=not args.no_budget)
    return 0


if __name__ == '__main__':
    sys.exit(main())
