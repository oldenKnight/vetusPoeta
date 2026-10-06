/* i18ncheck.js - string-table completeness (PREPLAN 4.4 item 6, PREDESIGN 5).
 *
 * check(opts) -> {errors:[], warnings:[], stats:{keys, used}}
 *   opts.i18nDir    directory with <lang>.json
 *   opts.langs      ['en-US', 'es-MX'] (first one is the reference)
 *   opts.jsFiles    files scanned for VP_I18n.t('literal'), *Key: 'literal', 'data-i18n*': 'literal'
 *   opts.htmlFiles  files scanned for data-i18n, -title, -placeholder, -aria, -static attributes
 *   opts.dynamic    parsed dynamic_keys.json
 *   opts.extraUsed  keys modules report through i18nKeys()
 */
'use strict';

var fs = require('fs');
var path = require('path');
var es5lint = require('./es5lint');

var SPAIN_ONLY = ['ordenador', 'ordenadores', 'fichero', 'ficheros', 'vale', 'vosotros', 'vosotras', 'coger', 'coge', 'cogí', 'cogido'];
var PLACEHOLDER = /\{(\w+)\}/g;
var KEY_NAME = /^[a-z][a-zA-Z0-9]*(\.[a-zA-Z0-9_]+)+$/;

function placeholders(s) {
  var out = [];
  var m;
  PLACEHOLDER.lastIndex = 0;
  while ((m = PLACEHOLDER.exec(s)) !== null) { if (out.indexOf(m[1]) < 0) { out.push(m[1]); } }
  return out.sort();
}

function loadTable(file, errors) {
  var raw;
  try { raw = fs.readFileSync(file, 'utf8'); } catch (e) { errors.push(path.basename(file) + ': cannot read (' + e.code + ')'); return null; }
  var data;
  try { data = JSON.parse(raw); } catch (e2) { errors.push(path.basename(file) + ': not valid JSON (' + e2.message + ')'); return null; }
  if (!data || typeof data !== 'object' || Array.isArray(data)) { errors.push(path.basename(file) + ': top level must be an object'); return null; }
  var name = path.basename(file);
  var seen = {};
  var re = /^\s*"((?:[^"\\]|\\.)*)"\s*:/gm;
  var m;
  while ((m = re.exec(raw)) !== null) {
    if (seen[m[1]]) { errors.push(name + ': duplicate key "' + m[1] + '"'); }
    seen[m[1]] = true;
  }
  var keys = Object.keys(data);
  for (var i = 1; i < keys.length; i++) {
    if (keys[i - 1] >= keys[i]) { errors.push(name + ': keys not sorted: "' + keys[i - 1] + '" comes before "' + keys[i] + '"'); break; }
  }
  keys.forEach(function (k) {
    var v = data[k];
    if (typeof v !== 'string' || !v.trim()) { errors.push(name + ': "' + k + '" is empty or not text'); }
    if (!KEY_NAME.test(k)) { errors.push(name + ': key "' + k + '" does not follow <area>.<part>... naming'); }
    if (typeof v === 'string' && /<\/?[a-zA-Z]/.test(v)) { errors.push(name + ': "' + k + '" contains HTML'); }
    if (typeof v === 'string' && v !== v.trim()) { errors.push(name + ': "' + k + '" has leading or trailing spaces'); }
  });
  return data;
}

// Areas of PREDESIGN 5.2: any string literal shaped like "<area>.<...>.<kind>" counts as used,
// which catches keys kept in lookup tables ({ready: 'app.status.engine.ready.label'}).
var KEY_LITERAL = /^(?:app|start|workspace|cue|source|target|inspector|why|engines|fidelity|names|corrections|words|orbergise|export|settings|about|tour|grammar|tier|confidence|error|toast|dialog|a11y|unit|key)\.[A-Za-z0-9_.]+\.(?:label|title|hint|placeholder|tooltip|aria|cta|confirm|empty|text|undone|one|other|zero)$/;

function usedInJs(src, used, prefixes) {
  var code = es5lint.strip(src, true).code;
  var res = [
    /(?:VP_I18n\.|[^\w$.])t\(\s*(['"])([A-Za-z0-9_.\-]+)\1(\s*\+)?/g,
    /[\w$]*Key\s*:\s*(['"])([A-Za-z0-9_.\-]+)\1(\s*\+)?/g,
    /['"]data-i18n(?:-title|-placeholder|-aria)?['"]\s*:\s*(['"])([A-Za-z0-9_.\-]+)\1(\s*\+)?/g
  ];
  res.forEach(function (re) {
    var m;
    while ((m = re.exec(code)) !== null) {
      if (m[3] || m[2].charAt(m[2].length - 1) === '.') { prefixes.push(m[2]); } else { used[m[2]] = true; }
    }
  });
  var lit = /(['"])([a-z][A-Za-z0-9_.]*)\1(\s*\+)?/g;
  var m2;
  while ((m2 = lit.exec(code)) !== null) {
    if (!m2[3] && KEY_LITERAL.test(m2[2])) { used[m2[2]] = true; }
  }
}

function usedInHtml(src, used, statics) {
  var re = /data-i18n(?:-title|-placeholder|-aria)?="([^"]+)"/g;
  var m;
  while ((m = re.exec(src)) !== null) { used[m[1]] = true; }
  var reS = /<(\w+)([^>]*?)\sdata-i18n-static="([^"]+)"([^>]*)>([^<]*)</g;
  while ((m = reS.exec(src)) !== null) {
    var attrs = m[2] + ' ' + m[4];
    var lang = /\slang="([^"]+)"/.exec(' ' + attrs);
    used[m[3]] = true;
    statics.push({ key: m[3], lang: lang ? lang[1] : null, text: m[5].trim() });
  }
}

function hasKey(dict, k) {
  return Object.prototype.hasOwnProperty.call(dict, k) ||
    (Object.prototype.hasOwnProperty.call(dict, k + '.one') && Object.prototype.hasOwnProperty.call(dict, k + '.other'));
}

function check(opts) {
  var errors = [];
  var warnings = [];
  var langs = opts.langs || ['en-US', 'es-MX'];
  var dynamic = opts.dynamic || { errorCodes: [], families: {} };
  var tables = {};
  langs.forEach(function (l) { tables[l] = loadTable(path.join(opts.i18nDir, l + '.json'), errors); });
  var ref = tables[langs[0]];
  if (!ref) { return { errors: errors, warnings: warnings, stats: { keys: 0, used: 0 } }; }

  langs.slice(1).forEach(function (l) {
    var t = tables[l];
    if (!t) { return; }
    var missing = Object.keys(ref).filter(function (k) { return !Object.prototype.hasOwnProperty.call(t, k); });
    var extra = Object.keys(t).filter(function (k) { return !Object.prototype.hasOwnProperty.call(ref, k); });
    if (missing.length) { errors.push(l + '.json is missing: ' + missing.join(', ')); }
    if (extra.length) { errors.push(l + '.json has keys ' + langs[0] + ' does not: ' + extra.join(', ')); }
    Object.keys(ref).forEach(function (k) {
      if (typeof t[k] !== 'string' || typeof ref[k] !== 'string') { return; }
      var a = placeholders(ref[k]).join(',');
      var b = placeholders(t[k]).join(',');
      if (a !== b) { errors.push(l + '.json: "' + k + '" placeholders {' + b + '} differ from ' + langs[0] + ' {' + a + '}'); }
    });
  });

  var es = tables['es-MX'];
  if (es) {
    Object.keys(es).forEach(function (k) {
      var v = es[k];
      if (typeof v !== 'string') { return; }
      var lower = v.toLowerCase();
      SPAIN_ONLY.forEach(function (w) {
        var re = new RegExp('(^|[^\\p{L}])' + w + '($|[^\\p{L}])', 'u');
        if (re.test(lower)) { errors.push('es-MX.json: "' + k + '" uses the Spain-only word "' + w + '"'); }
      });
      if (/[\p{L}\d)”"']\?/u.test(v) && v.indexOf('¿') < 0) { errors.push('es-MX.json: "' + k + '" ends a question with "?" but has no opening "¿"'); }
      if (/[\p{L}\d)”"']!/u.test(v) && v.indexOf('¡') < 0) { errors.push('es-MX.json: "' + k + '" ends an exclamation with "!" but has no opening "¡"'); }
    });
  }

  var used = {};
  var prefixes = [];
  var statics = [];
  (opts.jsFiles || []).forEach(function (f) { usedInJs(fs.readFileSync(f, 'utf8'), used, prefixes); });
  (opts.htmlFiles || []).forEach(function (f) { usedInHtml(fs.readFileSync(f, 'utf8'), used, statics); });
  (opts.extraUsed || []).forEach(function (k) { used[k] = true; });
  (dynamic.errorCodes || []).forEach(function (c) { used['error.' + c + '.title'] = true; used['error.' + c + '.hint'] = true; });

  Object.keys(used).sort().forEach(function (k) {
    if (!hasKey(ref, k)) { errors.push('the UI uses "' + k + '" but ' + langs[0] + '.json does not define it'); }
  });
  prefixes.forEach(function (p) {
    var any = Object.keys(ref).some(function (k) { return k.indexOf(p) === 0; });
    if (!any) { errors.push('the UI builds keys from "' + p + '" but no key starts with it'); }
  });
  statics.forEach(function (s) {
    var t = tables[s.lang || langs[0]];
    if (!t) { errors.push('data-i18n-static "' + s.key + '" has unknown lang "' + s.lang + '"'); return; }
    if (t[s.key] !== s.text) { errors.push('data-i18n-static "' + s.key + '" (' + (s.lang || langs[0]) + ') text differs from the table: "' + s.text + '"'); }
  });

  var families = Object.keys(dynamic.families || {});
  Object.keys(ref).forEach(function (k) {
    if (used[k]) { return; }
    var base = k.replace(/\.(one|other|zero)$/, '');
    if (used[base]) { return; }
    if (families.some(function (f) { return k.indexOf(f) === 0; })) { return; }
    if (prefixes.some(function (p) { return k.indexOf(p) === 0; })) { return; }
    warnings.push('key "' + k + '" is not used');
  });

  return { errors: errors, warnings: warnings, stats: { keys: Object.keys(ref).length, used: Object.keys(used).length } };
}

module.exports = { check: check, placeholders: placeholders, SPAIN_ONLY: SPAIN_ONLY };
