#!/usr/bin/env python3
"""Regenerate every derived brand asset in assets/ from the hand-written SVG sources.

Sources (edit these; docs/PREDESIGN.md 3.2 is the construction plan):
  assets/logo.svg         the full mark ("Arch and macron"), used from 40 px up
  assets/logo_small.svg   the reduced mark for 16, 20, 24 and 32 px
  assets/logo_grc.svg     the Greek variant (blue arch, perispomeni); not rasterised here

Outputs (do not edit by hand):
  assets/logo_wordmark.svg, logo_wordmark_dark.svg   mark + "vetus poeta" as outlines taken
                                                      from gui/ui/fonts/GentiumPlus-Bold.ttf
  assets/logo_256.png, logo_512.png, logo_1024.png   the mark on a transparent background
  assets/icon.ico                                    16 20 24 32 40 48 64 128 256, PNG entries
  assets/splash.png                                  1200x800 parchment, mark 160 px, name under it
  assets/HASHES.txt                                  SHA-256 of every output (and the sources)

Requirements: Python 3 standard library plus fontTools (for the wordmark outlines). Rasterising
is done by headless Chromium through tools/render_svg.js (Node + Playwright); there is no cairo
or Pillow here. If Node or Playwright is missing the script says so and stops.

Usage:
  python3 tools/make_icons.py            regenerate everything
  python3 tools/make_icons.py --verify   parse icon.ico and the PNGs with the standard library,
                                         check sizes, PNG headers and HASHES.txt; no browser
  python3 tools/make_icons.py --wordmark-only   only the two wordmark SVGs (no browser)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
FONT = ROOT / "gui" / "ui" / "fonts" / "GentiumPlus-Bold.ttf"
RENDER = ROOT / "tools" / "render_svg.js"

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SMALL_MAX = 32                  # sizes <= this come from logo_small.svg
PNG_SIZES = [256, 512, 1024]
SPLASH_W, SPLASH_H = 1200, 800
SPLASH_BG = "#FBF7F0"           # --bg light (PREDESIGN 2.1)
SPLASH_MARK = 160               # px
WORDMARK_TEXT = "vetus poeta"   # D1: lowercase, no macron
LETTER_SPACING_EM = 0.04
TEXT_LIGHT = "#2A2622"          # --text light
TEXT_DARK = "#F0E9DE"           # --text dark
HASHED = ["logo.svg", "logo_small.svg", "logo_grc.svg", "logo_wordmark.svg", "logo_wordmark_dark.svg",
          "logo_256.png", "logo_512.png", "logo_1024.png", "icon.ico", "splash.png"]


# ----------------------------------------------------------------------------- wordmark
def _num(v: float) -> str:
    s = "%.2f" % v
    s = s.rstrip("0").rstrip(".") if "." in s else s
    return "0" if s in ("-0", "") else s


class Kerning:
    """Pair kerning from the font's GPOS 'kern' feature (PairPos formats 1 and 2)."""

    def __init__(self, font):
        self.subtables = []
        if "GPOS" not in font:
            return
        gpos = font["GPOS"].table
        indices = set()
        for fr in gpos.FeatureList.FeatureRecord:
            if fr.FeatureTag == "kern":
                indices.update(fr.Feature.LookupListIndex)
        for li in sorted(indices):
            lookup = gpos.LookupList.Lookup[li]
            for st in lookup.SubTable:
                if lookup.LookupType == 9:
                    st = st.ExtSubTable
                if getattr(st, "LookupType", lookup.LookupType) == 2:
                    self.subtables.append(st)

    def __call__(self, a: str, b: str) -> int:
        for st in self.subtables:
            cov = st.Coverage.glyphs
            if a not in cov:
                continue
            if st.Format == 1:
                for rec in st.PairSet[cov.index(a)].PairValueRecord:
                    if rec.SecondGlyph == b:
                        return int(getattr(rec.Value1, "XAdvance", 0) or 0)
            elif st.Format == 2:
                c1 = st.ClassDef1.classDefs.get(a, 0)
                c2 = st.ClassDef2.classDefs.get(b, 0)
                rec = st.Class1Record[c1].Class2Record[c2]
                adv = int(getattr(rec.Value1, "XAdvance", 0) or 0)
                if adv:
                    return adv
        return 0


def text_outlines(text: str, size: float):
    """Returns (paths, advance, ymin, ymax, xheight) for `text` set in Gentium Plus Bold at
    `size` units per em, with letter spacing, kerning and y pointing down."""
    try:
        from fontTools.ttLib import TTFont
        from fontTools.pens.boundsPen import BoundsPen
        from fontTools.pens.svgPathPen import SVGPathPen
        from fontTools.pens.transformPen import TransformPen
    except ImportError:
        sys.exit("make_icons: fontTools is required for the wordmark (pip install fonttools)")
    font = TTFont(str(FONT))
    upem = font["head"].unitsPerEm
    scale = size / upem
    cmap = font.getBestCmap()
    glyphs = font.getGlyphSet()
    hmtx = font["hmtx"]
    kern = Kerning(font)
    names = [cmap[ord(ch)] for ch in text]
    paths = []
    x = 0.0
    ymin, ymax = 1e9, -1e9
    for i, name in enumerate(names):
        glyph = glyphs[name]
        pen = SVGPathPen(glyphs, ntos=_num)
        glyph.draw(TransformPen(pen, (scale, 0, 0, -scale, x, 0)))
        d = pen.getCommands()
        if d:
            paths.append(d)
            bp = BoundsPen(glyphs)
            glyph.draw(bp)
            gx0, gy0, gx1, gy1 = bp.bounds
            ymin = min(ymin, -gy1 * scale)
            ymax = max(ymax, -gy0 * scale)
        x += hmtx[name][0] * scale + LETTER_SPACING_EM * size
        if i + 1 < len(names):
            x += kern(name, names[i + 1]) * scale
    x -= LETTER_SPACING_EM * size          # no spacing after the last letter
    xheight = font["OS/2"].sxHeight * scale
    return paths, x, ymin, ymax, xheight


def mark_inner(svg_text: str) -> str:
    """The drawing elements of a mark SVG without wrapper, title, desc and comments."""
    body = re.search(r"<svg[^>]*>(.*)</svg>", svg_text, re.S).group(1)
    body = re.sub(r"<title>.*?</title>|<desc>.*?</desc>|<!--.*?-->", "", body, flags=re.S)
    return "\n".join("    " + ln.strip() for ln in body.splitlines() if ln.strip())


def build_wordmark(colour: str, title: str, with_mark: bool = True) -> str:
    """The lockup (mark + name); with_mark=False gives the name alone (used under the splash mark)."""
    size = 100.0                              # font units per em in the viewBox
    paths, adv, ymin, ymax, xheight = text_outlines(WORDMARK_TEXT, size)
    text_h = ymax - ymin
    tile_h = 1.6 * text_h                     # PREDESIGN 3.2 step 8: mark height = 1.6 x text height
    s = tile_h / 224.0                        # the tile is 224 of the 256 viewBox
    pad = 6.0
    gap = 0.32 * tile_h
    mark_x = pad
    mark_y = pad
    text_x = pad + tile_h + gap if with_mark else pad
    # centre the text block (ascender to descender) on the tile
    baseline = mark_y + tile_h / 2 - (ymin + ymax) / 2
    width = text_x + adv + pad
    height = tile_h + 2 * pad
    if not with_mark:
        baseline = pad - ymin
        height = text_h + 2 * pad
    mark = mark_inner((ASSETS / "logo.svg").read_text(encoding="utf-8"))
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%s" height="%s">'
           % (_num(width), _num(height), _num(width), _num(height)),
           "  <title>%s</title>" % title,
           "  <desc>The vetus poeta mark with the name set in Gentium Plus Bold, converted to outlines "
           "(letter-spacing 0.04 em). Generated by tools/make_icons.py; edit assets/logo.svg or the "
           "script, not this file.</desc>"]
    if with_mark:
        out += ['  <g transform="translate(%s %s) scale(%s)">' % (_num(mark_x - 16 * s), _num(mark_y - 16 * s), _num(s)),
                mark,
                "  </g>"]
    out.append('  <g fill="%s" transform="translate(%s %s)">' % (colour, _num(text_x), _num(baseline)))
    for d in paths:
        out.append('    <path d="%s"/>' % d)
    out += ["  </g>", "</svg>", ""]
    return "\n".join(out)


def write_wordmarks() -> None:
    for name, colour, title in (("logo_wordmark.svg", TEXT_LIGHT, "vetus poeta"),
                                ("logo_wordmark_dark.svg", TEXT_DARK, "vetus poeta (dark)")):
        text = build_wordmark(colour, title)
        (ASSETS / name).write_text(text, encoding="utf-8")
        print("  wrote %s (%d bytes)" % (ASSETS / name, len(text.encode("utf-8"))))


# ----------------------------------------------------------------------------- rendering
def render(jobs: list) -> None:
    if not RENDER.exists():
        sys.exit("make_icons: %s is missing" % RENDER)
    with tempfile.TemporaryDirectory() as tmp:
        jobs_file = Path(tmp) / "jobs.json"
        jobs_file.write_text(json.dumps(jobs), encoding="utf-8")
        try:
            proc = subprocess.run(["node", str(RENDER), str(jobs_file)], cwd=str(ROOT))
        except FileNotFoundError:
            sys.exit("make_icons: node is not installed; it is needed to drive Chromium through "
                     "tools/render_svg.js")
    if proc.returncode == 2:
        sys.exit("make_icons: Playwright or Chromium is missing (see the message above); set NODE_PATH "
                 "to a node_modules that has playwright and VP_CHROMIUM to a Chromium binary")
    if proc.returncode != 0:
        sys.exit("make_icons: rendering failed (exit %d)" % proc.returncode)


def splash_html(mark_svg: str, wordmark_svg: str) -> str:
    strip = lambda s: re.sub(r"^<\?xml[^>]*\?>\s*", "", s)
    return ("<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "html,body{margin:0;padding:0}"
            "body{width:%dpx;height:%dpx;background:%s;overflow:hidden;position:relative}"
            ".mark{position:absolute;left:50%%;top:232px;width:%dpx;height:%dpx;margin-left:-%dpx}"
            ".mark>svg{width:100%%;height:100%%;display:block}"
            ".word{position:absolute;left:50%%;top:440px;width:440px;margin-left:-220px}"
            ".word>svg{width:100%%;height:auto;display:block}"
            "</style></head><body><div class=\"mark\">%s</div><div class=\"word\">%s</div></body></html>"
            % (SPLASH_W, SPLASH_H, SPLASH_BG, SPLASH_MARK, SPLASH_MARK, SPLASH_MARK // 2,
               strip(mark_svg), strip(wordmark_svg)))


# ----------------------------------------------------------------------------- ico
def png_info(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    length, ctype = struct.unpack(">I4s", data[8:16])
    if ctype != b"IHDR" or length != 13:
        raise ValueError("bad IHDR")
    w, h, depth, colour = struct.unpack(">IIBB", data[16:26])
    crc = struct.unpack(">I", data[29:33])[0]
    if crc != zlib.crc32(data[12:29]) & 0xFFFFFFFF:
        raise ValueError("IHDR crc mismatch")
    return w, h, depth, colour


def write_ico(path: Path, pngs: list) -> None:
    """pngs: list of (size, bytes). Vista+ ICO with PNG-compressed 32-bit entries."""
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(header) + 16 * len(pngs)
    entries = b""
    for size, data in pngs:
        w, h, depth, colour = png_info(data)
        if (w, h) != (size, size) or depth != 8 or colour != 6:
            raise ValueError("entry %d: PNG is %dx%d depth %d colour %d (need RGBA 8 bit)" % (size, w, h, depth, colour))
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    path.write_bytes(header + entries + b"".join(data for _, data in pngs))


def read_ico(path: Path) -> list:
    data = path.read_bytes()
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or kind != 1:
        raise ValueError("not an ICO file")
    out = []
    for i in range(count):
        w, h, _, _, planes, bits, size, offset = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        w = w or 256
        h = h or 256
        blob = data[offset:offset + size]
        pw, ph, depth, colour = png_info(blob)
        if (pw, ph) != (w, h):
            raise ValueError("entry %d: directory says %dx%d, PNG is %dx%d" % (i, w, h, pw, ph))
        if depth != 8 or colour != 6 or bits != 32 or planes != 1:
            raise ValueError("entry %d: not a 32-bit RGBA PNG entry" % i)
        out.append(w)
    return out


# ----------------------------------------------------------------------------- hashes
def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_hashes() -> None:
    lines = ["%s  %s" % (sha256(ASSETS / n), n) for n in HASHED]
    (ASSETS / "HASHES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("  wrote %s" % (ASSETS / "HASHES.txt"))


def verify() -> int:
    ok = True
    ico = ASSETS / "icon.ico"
    try:
        sizes = read_ico(ico)
    except (ValueError, struct.error, OSError) as e:
        print("  FAIL icon.ico: %s" % e)
        return 1
    if sizes == ICO_SIZES:
        print("  ok   icon.ico entries %s (PNG-compressed, 32-bit RGBA)" % sizes)
    else:
        print("  FAIL icon.ico entries %s, expected %s" % (sizes, ICO_SIZES))
        ok = False
    for s in PNG_SIZES:
        p = ASSETS / ("logo_%d.png" % s)
        try:
            w, h, depth, colour = png_info(p.read_bytes())
            good = (w, h) == (s, s) and colour == 6
        except (ValueError, OSError) as e:
            print("  FAIL %s: %s" % (p.name, e))
            ok = False
            continue
        print("  %s %s %dx%d colour type %d" % ("ok  " if good else "FAIL", p.name, w, h, colour))
        ok = ok and good
    try:
        w, h, _, _ = png_info((ASSETS / "splash.png").read_bytes())
        good = (w, h) == (SPLASH_W, SPLASH_H)
        print("  %s splash.png %dx%d" % ("ok  " if good else "FAIL", w, h))
        ok = ok and good
    except (ValueError, OSError) as e:
        print("  FAIL splash.png: %s" % e)
        ok = False
    hashes = ASSETS / "HASHES.txt"
    if not hashes.exists():
        print("  FAIL HASHES.txt missing")
        return 1
    for line in hashes.read_text(encoding="utf-8").splitlines():
        digest, name = line.split("  ", 1)
        p = ASSETS / name
        if not p.exists():
            print("  FAIL %s missing" % name)
            ok = False
        elif sha256(p) != digest:
            print("  FAIL %s does not match HASHES.txt (regenerate: python3 tools/make_icons.py)" % name)
            ok = False
    if ok:
        print("  ok   HASHES.txt matches %d files" % len(HASHED))
    return 0 if ok else 1


# ----------------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verify", action="store_true", help="check icon.ico, the PNGs and HASHES.txt; no browser")
    ap.add_argument("--wordmark-only", action="store_true", help="only write the wordmark SVGs")
    args = ap.parse_args()
    if args.verify:
        return verify()

    print("wordmarks")
    write_wordmarks()
    if args.wordmark_only:
        return 0

    logo = str(ASSETS / "logo.svg")
    small = str(ASSETS / "logo_small.svg")
    with tempfile.TemporaryDirectory() as tmp:
        jobs = []
        for s in PNG_SIZES:
            jobs.append({"svg": logo, "w": s, "h": s, "out": str(ASSETS / ("logo_%d.png" % s))})
        for s in ICO_SIZES:
            jobs.append({"svg": small if s <= SMALL_MAX else logo, "w": s, "h": s,
                         "out": os.path.join(tmp, "ico_%d.png" % s)})
        html = splash_html((ASSETS / "logo.svg").read_text(encoding="utf-8"),
                           build_wordmark(TEXT_LIGHT, "vetus poeta", with_mark=False))
        jobs.append({"html": html, "w": SPLASH_W, "h": SPLASH_H, "out": str(ASSETS / "splash.png")})
        print("rendering %d images with Chromium" % len(jobs))
        render(jobs)
        pngs = [(s, Path(tmp, "ico_%d.png" % s).read_bytes()) for s in ICO_SIZES]
        write_ico(ASSETS / "icon.ico", pngs)
        print("  wrote %s entries=%s (%d bytes)" % (ASSETS / "icon.ico", ICO_SIZES, (ASSETS / "icon.ico").stat().st_size))
    write_hashes()
    print("verify")
    return verify()


if __name__ == "__main__":
    sys.exit(main())
