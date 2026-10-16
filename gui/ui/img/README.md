# gui/ui/img - the UI icon sprite

`icons.svg` is an SVG symbol sprite of the simple icons the screens use or will use. Every
symbol is original artwork drawn for this app on a 24 x 24 grid with 2 px coordinates, drawn
with `currentColor` (2 px round strokes; the four cue shapes `dot`, `circle`, `triangle`,
`square` are filled). The file ships with the UI (`tools/pack_ui.py` copies everything that is
not `.md`/`.py`); this README does not.

Symbols: `leaf` (tier badge), `undo`, `redo`, `gear`, `question`, `search`, `filter`, `lock`,
`dot`, `circle`, `triangle`, `square` (cue confidence shapes: OK / Check / Fix), `play`
(preview), `export`, `folder`, `file-text`, `drop-arrow` (drop zone), `copy`, `trash`,
`chevron-down`, `chevron-right`, `close`, `check`, `warning`, `offline` (monitor), `online`
(plug), `moon`, `sun`.

## Use

    <svg class="vp-icon" aria-hidden="true" focusable="false"><use href="img/icons.svg#undo"/></svg>

With `VP_Dom.el`: `D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' },
[D.el('use', { href: 'img/icons.svg#undo' })])`. The path is relative to `index.html`. The page
CSP (`default-src 'self'`) allows the same-origin fetch of the sprite, both from the dev server
and from the shell's virtual host; `file://` opens may block it (same as the fonts). The
existing `.vp-icon` rule (16 px, `stroke: currentColor`, width 1.75) applies; add
`vp-icon-24` for the 24 px size. The colour is the element's `color`, so `color: var(--ok)`
tints a symbol. Symbols carry their own `viewBox`, so the `<svg>` needs only a size.

The inline sprite in `index.html` (`#vp-i-leaf`, `#vp-i-close`, `#vp-i-online`,
`#vp-i-offline`) and the `svgIcon()` paths inside `vp_start.js` / `vp_workspace.js` /
`vp_cuelist.js` stay as they are until the UI owner switches them to `img/icons.svg`; the
sprite has the same symbols (`leaf`, `close`, `online`, `offline`, `gear`, `question`,
`file-text`, `undo`, `redo`, `lock`) so that switch is a one-line change per icon.

Rendering check: `node tools/check_brand.js` draws every symbol at 24 and 48 px and in the
accent colour into `assets/out/ui_icons.png`.
