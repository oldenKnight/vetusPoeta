/* checks.js - static checks of index.html and the CSS (DESIGN 13, PREDESIGN 2.1 and 6.2).
 * html(file, jsDir) -> [problem]; css(cssDir) -> {problems:[], info:{...}}
 */
'use strict';

var fs = require('fs');
var path = require('path');
var loader = require('./loader');

var CSP = "default-src 'self'; img-src 'self' data:; style-src 'self'; font-src 'self'";
var CSS_ORDER = ['css/tokens.css', 'css/base.css', 'css/components.css', 'css/screens.css'];
var CSS_BUDGET = 60 * 1024;

// PREDESIGN 2.1, light and dark, exactly.
var TOKENS = {
  '--bg': ['#FBF7F0', '#1B1815'], '--surface': ['#FFFFFF', '#25211D'], '--surface-2': ['#F3ECDF', '#2E2924'],
  '--border': ['#D9CDB8', '#3F3830'], '--border-strong': ['#8A7F6D', '#8C8173'], '--text': ['#2A2622', '#F0E9DE'],
  '--muted': ['#6B6157', '#B5AA9B'], '--accent': ['#B3452A', '#E8825F'], '--on-accent': ['#FFFFFF', '#1B1815'],
  '--greek': ['#2B6A9B', '#86B8E3'], '--ok': ['#2F6F44', '#7FC48F'], '--warn': ['#8A5A00', '#E6B35C'],
  '--bad': ['#A8322B', '#F08A80'], '--focus': ['#1F5FBF', '#8DB8FF'], '--accent-soft': ['#F6DDD3', '#3A2620']
};

function html(file, jsDir) {
  var problems = [];
  var src = fs.readFileSync(file, 'utf8');
  var dir = path.dirname(file);
  if (!/^<!doctype html>/i.test(src)) { problems.push('missing <!doctype html>'); }
  if (!/<html lang="en-US">/.test(src)) { problems.push('<html lang="en-US"> missing'); }
  var csp = /<meta http-equiv="Content-Security-Policy" content="([^"]*)">/.exec(src);
  if (!csp) { problems.push('CSP meta missing'); } else if (csp[1] !== CSP) { problems.push('CSP differs from DESIGN 13: "' + csp[1] + '"'); }
  ['header', 'main', 'aside', 'footer', 'noscript'].forEach(function (tag) {
    if (src.indexOf('<' + tag) < 0) { problems.push('<' + tag + '> missing'); }
  });
  if (!/id="app"/.test(src)) { problems.push('#app root missing'); }
  if (/<style[\s>]/i.test(src)) { problems.push('inline <style> (blocked by the CSP)'); }
  if (/\sstyle\s*=/i.test(src)) { problems.push('style="" attribute (blocked by the CSP)'); }
  if (/\son[a-z]+\s*=/i.test(src)) { problems.push('inline on* event handler attribute'); }
  var ids = {};
  var idRe = /\sid="([^"]+)"/g;
  var m;
  while ((m = idRe.exec(src)) !== null) {
    if (ids[m[1]]) { problems.push('duplicate id "' + m[1] + '"'); }
    ids[m[1]] = true;
  }
  var scripts = [];
  var sRe = /<script\b([^>]*)>([\s\S]*?)<\/script>/gi;
  while ((m = sRe.exec(src)) !== null) {
    var srcAttr = /\ssrc="([^"]+)"/.exec(m[1]);
    if (!srcAttr) { problems.push('inline <script> (no src)'); continue; }
    if (m[2].trim()) { problems.push('<script src="' + srcAttr[1] + '"> has a body'); }
    scripts.push(srcAttr[1]);
  }
  var last = -1;
  scripts.forEach(function (s) {
    if (!fs.existsSync(path.join(dir, s))) { problems.push('script ' + s + ' does not exist'); }
    var base = path.basename(s);
    var idx = loader.tableIndex(base);
    if (s !== 'js/' + base) { problems.push('script ' + s + ' is not under js/'); }
    if (idx < 0) { problems.push('script ' + s + ' is not in DESIGN 13'); return; }
    if (idx <= last) { problems.push('script ' + s + ' is out of DESIGN 13 order'); }
    last = idx;
  });
  loader.presentFiles(jsDir).forEach(function (f) {
    if (scripts.indexOf('js/' + f) < 0) { problems.push('js/' + f + ' exists but index.html does not load it'); }
  });
  var links = [];
  var lRe = /<link rel="stylesheet" href="([^"]+)">/g;
  while ((m = lRe.exec(src)) !== null) { links.push(m[1]); }
  var expected = CSS_ORDER.filter(function (c) { return fs.existsSync(path.join(dir, c)); });
  if (links.join(',') !== expected.join(',')) { problems.push('stylesheets must be ' + expected.join(', ') + ' in that order; found ' + links.join(', ')); }
  links.forEach(function (l) { if (!fs.existsSync(path.join(dir, l))) { problems.push('stylesheet ' + l + ' does not exist'); } });
  var pre = /<link rel="preload" href="([^"]+)" as="font"/.exec(src);
  if (pre && !fs.existsSync(path.join(dir, pre[1]))) { problems.push('preloaded font ' + pre[1] + ' does not exist'); }
  var bRe = /<button\b([^>]*)>/g;
  while ((m = bRe.exec(src)) !== null) {
    if (!/data-i18n|aria-label/.test(m[1])) { problems.push('<button> without data-i18n or aria-label'); }
  }
  return { problems: problems, scripts: scripts.length };
}

function hexRgb(hex) {
  var h = hex.replace('#', '');
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}
function luminance(hex) {
  var c = hexRgb(hex).map(function (v) {
    v /= 255;
    return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}
function contrast(a, b) {
  var la = luminance(a);
  var lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

function block(src, selector) {
  var i = src.indexOf(selector + ' {');
  if (i < 0) { return null; }
  var j = src.indexOf('}', i);
  var out = {};
  var re = /(--[\w-]+)\s*:\s*([^;]+);/g;
  var body = src.slice(i, j);
  var m;
  while ((m = re.exec(body)) !== null) { out[m[1]] = m[2].trim().replace(/\s+/g, ' '); }
  return out;
}

function css(cssDir) {
  var problems = [];
  var files = fs.readdirSync(cssDir).filter(function (f) { return /\.css$/.test(f); });
  var total = 0;
  files.forEach(function (f) { total += fs.statSync(path.join(cssDir, f)).size; });
  if (total > CSS_BUDGET) { problems.push('CSS is ' + total + ' bytes, budget ' + CSS_BUDGET); }
  var tokens = fs.readFileSync(path.join(cssDir, 'tokens.css'), 'utf8');
  var light = block(tokens, ':root');
  var dark = block(tokens, ':root[data-theme="dark"]');
  var auto = block(tokens, ':root:not([data-theme="light"])');
  if (!light || !dark || !auto) {
    problems.push('tokens.css needs :root, :root[data-theme="dark"] and @media dark :root:not([data-theme="light"]) blocks');
    return { problems: problems, info: { bytes: total, files: files.length } };
  }
  if (!/@media \(prefers-color-scheme: dark\)\s*\{\s*:root:not\(\[data-theme="light"\]\)/.test(tokens)) { problems.push('dark auto block is not inside @media (prefers-color-scheme: dark)'); }
  Object.keys(TOKENS).forEach(function (t) {
    if ((light[t] || '').toUpperCase() !== TOKENS[t][0]) { problems.push('light ' + t + ' is ' + light[t] + ', PREDESIGN 2.1 says ' + TOKENS[t][0]); }
    if ((dark[t] || '').toUpperCase() !== TOKENS[t][1]) { problems.push('dark ' + t + ' is ' + dark[t] + ', PREDESIGN 2.1 says ' + TOKENS[t][1]); }
  });
  Object.keys(dark).forEach(function (t) { if (auto[t] !== dark[t]) { problems.push('auto-dark ' + t + ' differs from [data-theme="dark"]'); } });
  Object.keys(auto).forEach(function (t) { if (dark[t] === undefined) { problems.push('auto-dark defines ' + t + ' but [data-theme="dark"] does not'); } });
  ['--font-text', '--font-ui', '--font-emoji', '--font-mono', '--text-scale'].forEach(function (t) {
    if (!light[t]) { problems.push(':root misses ' + t); }
  });
  if (light['--font-text'] && light['--font-text'].indexOf('"Gentium Plus"') !== 0) { problems.push('--font-text must start with "Gentium Plus"'); }
  var faces = tokens.match(/@font-face\s*\{[^}]*\}/g) || [];
  ['GentiumPlus-Regular.ttf', 'GentiumPlus-Italic.ttf', 'GentiumPlus-Bold.ttf'].forEach(function (f) {
    var face = faces.filter(function (x) { return x.indexOf(f) >= 0; })[0];
    if (!face) { problems.push('@font-face for ' + f + ' missing'); return; }
    if (face.indexOf('font-display: swap') < 0) { problems.push('@font-face ' + f + ' lacks font-display: swap'); }
    if (!fs.existsSync(path.join(cssDir, '..', 'fonts', f))) { problems.push('fonts/' + f + ' missing'); }
  });

  var pairs = 0;
  var worst = 99;
  function need(theme, fg, bg, min) {
    var a = theme[fg] || (fg.charAt(0) === '#' ? fg : null);
    var b = theme[bg];
    if (!a || !b || a.charAt(0) !== '#' || b.charAt(0) !== '#') { problems.push('cannot check contrast ' + fg + ' on ' + bg); return; }
    var r = contrast(a, b);
    pairs++;
    if (min === 4.5) { worst = Math.min(worst, r); }
    if (r < min) { problems.push((theme === light ? 'light' : 'dark') + ' ' + fg + ' on ' + bg + ' is ' + r.toFixed(2) + ':1, needs ' + min + ':1'); }
  }
  [light, dark].forEach(function (th) {
    ['--text', '--muted', '--accent', '--greek', '--ok', '--warn', '--bad'].forEach(function (fg) {
      ['--bg', '--surface', '--surface-2'].forEach(function (bg) { need(th, fg, bg, 4.5); });
    });
    need(th, '--on-accent', '--accent', 4.5);
    need(th, '--on-accent', '--greek', 4.5);
    need(th, '--text', '--accent-soft', 4.5);
    need(th, '--text', '--greek-soft', 4.5);
    need(th, '--bg', '--text', 4.5);
    need(th, '--border-strong', '--bg', 3);
    need(th, '--border-strong', '--surface', 3);
    need(th, '--focus', '--bg', 3);
    need(th, '--focus', '--surface', 3);
  });
  return { problems: problems, info: { bytes: total, files: files.length, tokens: Object.keys(TOKENS).length, pairs: pairs, worst: worst } };
}

module.exports = { html: html, css: css, contrast: contrast, CSP: CSP };
