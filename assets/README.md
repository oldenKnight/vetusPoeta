# assets - the vetus poeta brand

Original artwork made for this program; **proprietary, part of the program** (same terms as the
source). No third-party logo, clip art, font-as-logo or character is traced, copied or imitated: the
mark is drawn from the geometric primitives of the construction plan in `docs/PREDESIGN.md` 3.2
(concept A, "Arch and macron"), the icons in `gui/ui/img/icons.svg` are simple original strokes, and
the wordmark letters are outlines of Gentium Plus Bold (see the OFL note below).

## Sources (hand-written SVG, edit these)

| File | What it is |
|---|---|
| `logo.svg` | The full mark, viewBox 256: ink-blue tile `#22344A` (224 px, radius 52), terracotta `#E2704A` arch (stroke 26, feet flat at y 196), threshold step, gold `#E8B84A` macron floating 15 px above the arch (0.68 of the arch width, so it reads as a vowel mark, not a lintel), parchment `#FBF7F0` quill leaf inside the arch with a rachis cut in the tile colour and a nib resting on the threshold. Used from 40 px up. |
| `logo_small.svg` | The reduced mark for 16-32 px (plan step 7): tile, arch with stroke 32, macron 18 px high; no threshold, no quill. |
| `logo_grc.svg` | The Greek variant (plan step 6): arch and threshold in Aegean blue `#86B8E3`, a gold perispomeni-style tilde instead of the macron, quill unchanged. For the Greek pairs in the workspace and a Greek installer tile; not rasterised by the generator. |

Adjustments to the plan's numbers, made after rendering and looking (all inside the plan's intent):

* **Quill leaf.** The plan's leaf (`M112 190 C100 150 118 112 150 98 ...`) has its tip at (150, 98),
  which is 9 px below the inner apex *line* but only 1.8 px from the inner arc itself (the tip sits
  off-centre, where the arc is already curving down), so the tip touched the arch. The leaf was
  scaled to 0.88, rotated 2 degrees back, moved to base (110, 184) and widened a little:
  `M110 184 C96 149 113 115 141 102 C149 133 136 164 110 184 Z`, rachis cut `110 184 -> 137 112`,
  nib `110 184 -> 104 198`. Measured on the curve: 9.9 px minimum clearance to the inner arc, lean
  20.7 degrees to the right, tip 13 px below the inner apex line.
* **Greek tilde.** The plan's path (`q10 -14 20 0 t20 0 t20 0 t20 0`, four humps) rendered as a water
  wave. It is now one tilde, `M88 46 q20 -18 40 0 t40 0` (same width 80, same stroke 7 round), which
  reads as a perispomeni.
* **Small macron.** Height 18 as planned, placed at y 28-46 (bottom edge 2 px higher than the full
  mark's) so the gap to the heavier arch (stroke 32, apex outer edge at y 60) stays 14 px and is still
  a visible gap at 16 px.
* **Wordmark lockup.** Step 8's "mark height = 1.6 x-height x 4" was read as: tile height = 1.6 x the
  text's ascender-to-descender height (the `t` top to the `p` descender). Gap between tile and text
  0.32 x tile height; the text block is centred vertically on the tile.

## Generated (do not edit; run `python3 tools/make_icons.py`)

| File | Contents |
|---|---|
| `logo_wordmark.svg`, `logo_wordmark_dark.svg` | The lockup: the mark at left plus `vetus poeta` (D1: lowercase, no macron) set in Gentium Plus Bold with 0.04 em letter spacing and the font's own kerning, converted to outlines. Text colour `#2A2622` (light `--text`) / `#F0E9DE` (dark `--text`); the mark keeps its own colours in both. viewBox 732 x 150. |
| `logo_256.png`, `logo_512.png`, `logo_1024.png` | `logo.svg` on a transparent background (RGBA). |
| `icon.ico` | Windows icon for `VetusPoeta.exe` (`gui/shell/VetusPoeta.rc.in`): entries 16, 20, 24, 32 (from `logo_small.svg`) and 40, 48, 64, 128, 256 (from `logo.svg`), each a PNG-compressed 32-bit RGBA entry (Vista and later). Replaces the shell task's placeholder leaf; `gui/shell/tools/make_placeholder_icon.py` is no longer used. |
| `splash.png` | 1200 x 800 on parchment `#FBF7F0`: the mark at 160 px, the name (text part of the wordmark, 440 px wide) under it. |
| `HASHES.txt` | SHA-256 of the three sources and every output. |

How it is made: `tools/make_icons.py` (Python 3 standard library + fontTools) writes the wordmark SVGs
(fontTools reads the glyph outlines, GPOS `kern` pairs and metrics of
`gui/ui/fonts/GentiumPlus-Bold.ttf`), then rasterises with headless Chromium through
`tools/render_svg.js` (Node + Playwright; there is no cairo or Pillow in the build box), assembles the
`.ico` with `struct`, writes `HASHES.txt` and runs `--verify`. Exact commands:

    python3 tools/make_icons.py              # everything (needs node + Playwright + a Chromium)
    python3 tools/make_icons.py --verify     # standard-library parse of icon.ico, PNG headers, HASHES.txt
    python3 tools/make_icons.py --wordmark-only
    node tools/check_brand.js                # dev renders into assets/out/ (gitignored), see below

Playwright is looked up in `NODE_PATH`, then `/opt/node-tools/node_modules`; Chromium in `VP_CHROMIUM`,
then the Playwright default, then `$PLAYWRIGHT_BROWSERS_PATH` (default `/opt/pw-browsers`). The output
is deterministic for a given Chromium build (two runs here gave identical `HASHES.txt`); another
Chromium may encode the PNGs differently, which changes the hashes but not the pictures.

## Licence notes

* Artwork (`logo*.svg`, the PNGs, `icon.ico`, `splash.png`, `gui/ui/img/icons.svg`): proprietary, part
  of the program; not for reuse outside it.
* The wordmark text is **outlined glyphs of Gentium Plus Bold** (SIL Open Font License 1.1, Reserved
  Font Names "Gentium" and "SIL"). The OFL permits using the glyph outlines in artwork; no font file is
  embedded in, derived from or distributed by these SVGs, nothing is renamed, and the font itself ships
  unmodified in `gui/ui/fonts/` with its `OFL.txt` (DESIGN 14).

## Rendering check (`node tools/check_brand.js`, looked at on 2026-10-06)

| Render (`assets/out/`) | Verdict |
|---|---|
| `logo_256.png` | Arch, macron and quill all read; the quill is a feather with rachis and nib, 10 px clear of the arch; the macron floats and reads as a long mark, not a lintel. |
| `logo_48.png` | Still readable with the quill as a light sliver; the smallest size the full mark is used at is 40. |
| `logo_small_16.png` | Tile, terracotta arch and gold bar survive as three distinct shapes with a visible gap. |
| `logo_small_24.png`, `logo_small_32.png` | Clean arch and bar, rounded tile corners visible. |
| `logo_small_256.png` | The reduced mark is balanced on its own (heavier arch, taller bar). |
| `logo_grc_256.png` | Blue arch and gold tilde read as "Greek"; the tilde is one perispomeni. |
| `logo_grc_32.png` | Tilde and arch legible, the quill blurs (the Greek mark is not meant for icon sizes; `logo_small` is). |
| `wordmark_light.png`, `wordmark_dark.png` | Lockup balanced on parchment and on the dark background; letter spacing even, kerning applied (`et`, `ta`). |
| `wordmark_topbar_28px.png` | Readable at the top bar's 28 px height. |
| `ui_icons.png` | All 28 symbols distinct at 24 px and in the accent colour. |
| `splash.png` (in `assets/`) | Mark centred above the name on parchment, nothing else. |

## Hashes (`HASHES.txt`, SHA-256)

```
e3af671734c300b79e296f8e66696c59847139caceb09d6e7eaf37ce22432585  logo.svg
401c977cdb9359909868826b6a3f643c55ad4b650ca8bec784381976eeee03b8  logo_small.svg
ad8398b8c074087b5082973bb525a5cff5d54df47aa3759ca7ff1cbea0ce4200  logo_grc.svg
e6860dbe6332946b96e7943b5e5a43ede1d2cbd21cefce0de5e6c8ec61d69181  logo_wordmark.svg
af6213460d60e50b383959260971f976a85380f3d824413fe05fa1b25503b224  logo_wordmark_dark.svg
f78958eef744b88192cb71c9151f23d015a2e721bd9d8130ae1c456495fe0b9d  logo_256.png
c50625a954a2904496659fa6c7cfd2806898ea2f9bc93c2bd5a718e21180d717  logo_512.png
74fdf17a631d49bb0842b1374a700786719ca9d8347577ea56ab5509b058c45c  logo_1024.png
fa3b8eee5713b95c874879805c416c40aceafeae6bfb010db9a53d2a097d1032  icon.ico
```
