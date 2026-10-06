/* pseudo.js - the xx-LONG pseudo-locale (PREDESIGN 5.2): every vowel doubled, {placeholders}
 * and [b]..[/b] style tokens untouched, wrapped in brackets so clipped text is visible.
 * Used only by tests (never shipped).
 */
'use strict';

function longText(s) {
  var out = '';
  var re = /\{\w+\}|\[\/?\w+\]/g;
  var last = 0;
  var m;
  while ((m = re.exec(s)) !== null) {
    out += s.slice(last, m.index).replace(/[aeiouAEIOUáéíóúÁÉÍÓÚ]/g, function (v) { return v + v; });
    out += m[0];
    last = m.index + m[0].length;
  }
  out += s.slice(last).replace(/[aeiouAEIOUáéíóúÁÉÍÓÚ]/g, function (v) { return v + v; });
  return '[' + out + ']';
}

function make(dict) {
  var out = {};
  Object.keys(dict).forEach(function (k) { out[k] = longText(dict[k]); });
  return out;
}

module.exports = { make: make, longText: longText };
