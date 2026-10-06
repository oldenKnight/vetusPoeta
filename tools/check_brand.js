/* check_brand.js - DEV rendering check of the brand assets (task C4). Renders the marks and
 * wordmarks with headless Chromium through tools/render_svg.js into assets/out/ (gitignored),
 * checks that every PNG exists with the expected pixel size and prints the sizes, so a person
 * can open the renders and look at them. Prints SKIP and exits 0 when Playwright or Chromium
 * is missing; exits 1 on any failed render.
 *   node tools/check_brand.js
 * Renders: logo.svg at 256 and 48; logo_small.svg at 16, 24, 32 and 256; logo_grc.svg at
 * 256 and 32; the wordmarks on --bg light (#FBF7F0) and dark (#1B1815) and the light one
 * at top-bar height (28 px); the UI icon sprite gui/ui/img/icons.svg as a labelled grid.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var child = require('child_process');

var ROOT = path.resolve(__dirname, '..');
var ASSETS = path.join(ROOT, 'assets');
var OUT = path.join(ASSETS, 'out');
var SPRITE = path.join(ROOT, 'gui', 'ui', 'img', 'icons.svg');

function job(svg, w, h, out, bg, pad) {
  return { svg: path.join(ASSETS, svg), w: w, h: h, out: path.join(OUT, out), bg: bg, pad: pad || 0 };
}

function spriteHtml(w, h) {
  var sprite = fs.readFileSync(SPRITE, 'utf8').replace(/style="display:none"/, 'style="position:absolute;width:0;height:0"');
  var ids = [];
  sprite.replace(/<symbol id="([^"]+)"/g, function (m, id) { ids.push(id); return m; });
  var cells = ids.map(function (id) {
    return '<div class="c"><svg class="i24"><use href="#' + id + '"/></svg><svg class="i72"><use href="#' + id + '"/></svg>' +
      '<svg class="i24 a"><use href="#' + id + '"/></svg><span>' + id + '</span></div>';
  }).join('');
  return '<!doctype html><html><head><meta charset="utf-8"><style>' +
    'html,body{margin:0;padding:0}body{width:' + w + 'px;height:' + h + 'px;background:#FBF7F0;color:#2A2622;' +
    'font:12px "Noto Sans","DejaVu Sans",sans-serif;overflow:hidden}' +
    '.g{display:flex;flex-wrap:wrap;padding:12px;gap:12px}' +
    '.c{width:150px;height:96px;display:flex;align-items:center;gap:8px;background:#fff;border:1px solid #D9CDB8;border-radius:8px;padding:8px;box-sizing:border-box}' +
    '.i24{width:24px;height:24px;flex:none}.i72{width:48px;height:48px;flex:none}.a{color:#B3452A}' +
    'span{font-size:11px;overflow:hidden}' +
    '</style></head><body>' + sprite + '<div class="g">' + cells + '</div></body></html>';
}

function pngSize(file) {
  var b = fs.readFileSync(file);
  if (b.length < 24 || b.toString('latin1', 1, 4) !== 'PNG') { return null; }
  return { w: b.readUInt32BE(16), h: b.readUInt32BE(20) };
}

function main() {
  if (!fs.existsSync(OUT)) { fs.mkdirSync(OUT, { recursive: true }); }
  var jobs = [
    job('logo.svg', 256, 256, 'logo_256.png'),
    job('logo.svg', 48, 48, 'logo_48.png'),
    job('logo_small.svg', 16, 16, 'logo_small_16.png'),
    job('logo_small.svg', 24, 24, 'logo_small_24.png'),
    job('logo_small.svg', 32, 32, 'logo_small_32.png'),
    job('logo_small.svg', 256, 256, 'logo_small_256.png'),
    job('logo_grc.svg', 256, 256, 'logo_grc_256.png'),
    job('logo_grc.svg', 32, 32, 'logo_grc_32.png'),
    job('logo_wordmark.svg', 760, 180, 'wordmark_light.png', '#FBF7F0', 12),
    job('logo_wordmark_dark.svg', 760, 180, 'wordmark_dark.png', '#1B1815', 12),
    job('logo_wordmark.svg', 160, 36, 'wordmark_topbar_28px.png', '#FFFFFF', 4)
  ];
  if (fs.existsSync(SPRITE)) {
    jobs.push({ html: spriteHtml(1000, 560), w: 1000, h: 560, out: path.join(OUT, 'ui_icons.png') });
  }
  var jobsFile = path.join(OUT, 'jobs.json');
  fs.writeFileSync(jobsFile, JSON.stringify(jobs));
  var r = child.spawnSync('node', [path.join(__dirname, 'render_svg.js'), jobsFile], { stdio: ['ignore', 'pipe', 'pipe'] });
  process.stdout.write(String(r.stdout || ''));
  if (r.status === 2) {
    process.stdout.write('check_brand: SKIP (' + String(r.stderr || '').trim().split('\n')[0] + ')\n');
    return 0;
  }
  if (r.status !== 0) {
    process.stderr.write(String(r.stderr || ''));
    process.stdout.write('check_brand: FAIL (renderer exit ' + r.status + ')\n');
    return 1;
  }
  var bad = 0;
  jobs.forEach(function (j) {
    var s = fs.existsSync(j.out) ? pngSize(j.out) : null;
    var ok = s && s.w === j.w && s.h === j.h;
    if (!ok) { bad++; }
    process.stdout.write('  ' + (ok ? 'ok  ' : 'FAIL') + ' ' + path.relative(ROOT, j.out) + ' ' +
      (s ? s.w + 'x' + s.h : 'missing') + ' (' + (fs.existsSync(j.out) ? fs.statSync(j.out).size : 0) + ' bytes)\n');
  });
  fs.unlinkSync(jobsFile);
  process.stdout.write(bad ? 'check_brand: FAIL (' + bad + ' renders)\n' : 'check_brand: OK, ' + jobs.length + ' renders in assets/out/ (look at them)\n');
  return bad ? 1 : 0;
}

process.exit(main());
