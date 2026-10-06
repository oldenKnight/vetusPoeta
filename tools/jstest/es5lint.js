/* es5lint.js - ES5-only lint for the vetus poeta UI (PREPLAN 4.4 item 3).
 *
 * strip(src) walks the source once with a small state machine and blanks out comments,
 * string contents, regex-literal bodies and template literals (keeping every newline and
 * every column, so positions still match the file). The banned-construct rules then run
 * on code only, so "=>" inside a string or a comment is never reported.
 *
 * lint(src, opts) -> [{rule, line, col, text}]
 *   opts.profile: 'ui' (gui/ui/js), 'test' (gui/ui/tests), 'node' (tools, dev scripts:
 *   syntax rules only, Node APIs allowed); opts.file: base name, for per-file exceptions.
 */
'use strict';

var REGEX_PREV_WORDS = {
  'return': 1, 'typeof': 1, 'instanceof': 1, 'in': 1, 'of': 1, 'new': 1, 'delete': 1,
  'void': 1, 'throw': 1, 'case': 1, 'do': 1, 'else': 1, 'yield': 1, 'await': 1
};

function isIdentChar(c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
    c === '_' || c === '$' || c > '\u007f';
}

// Returns {code, templates:[index...], problems:[{kind, index}]}.
// keepStrings: blank comments only (used by the i18n scanner, which needs string literals).
function strip(src, keepStrings) {
  var out = src.split('');
  var n = src.length;
  var i = 0;
  var templates = [];
  var problems = [];
  var lastSig = '';     // last significant (non-space) character of code
  var lastWord = '';    // last identifier/keyword if lastSig ended one

  function blank(from, to) {   // [from, to)
    for (var k = from; k < to; k++) {
      if (out[k] !== '\n' && out[k] !== '\r') { out[k] = ' '; }
    }
  }

  while (i < n) {
    var c = src[i];
    var d = i + 1 < n ? src[i + 1] : '';
    if (c === '/' && d === '/') {
      var e = src.indexOf('\n', i);
      if (e < 0) { e = n; }
      blank(i, e);
      i = e;
      continue;
    }
    if (c === '/' && d === '*') {
      var e2 = src.indexOf('*/', i + 2);
      if (e2 < 0) { problems.push({ kind: 'comment', index: i }); e2 = n - 2; }
      blank(i, e2 + 2);
      i = e2 + 2;
      continue;
    }
    if (c === '"' || c === '\'') {
      var j = i + 1;
      while (j < n && src[j] !== c) {
        if (src[j] === '\\') { j++; } else if (src[j] === '\n') { break; }
        j++;
      }
      if (j >= n || src[j] !== c) { problems.push({ kind: 'string', index: i }); }
      if (!keepStrings) { blank(i + 1, Math.min(j, n)); }
      lastSig = c; lastWord = '';
      i = j + 1;
      continue;
    }
    if (c === '`') {
      templates.push(i);
      var depth = 0;
      var k2 = i + 1;
      while (k2 < n) {
        if (src[k2] === '\\') { k2 += 2; continue; }
        if (depth === 0 && src[k2] === '`') { break; }
        if (src[k2] === '$' && src[k2 + 1] === '{') { depth++; k2 += 2; continue; }
        if (depth > 0 && src[k2] === '}') { depth--; }
        k2++;
      }
      if (k2 >= n) { problems.push({ kind: 'template', index: i }); }
      if (!keepStrings) { blank(i + 1, Math.min(k2, n)); }
      lastSig = '`'; lastWord = '';
      i = k2 + 1;
      continue;
    }
    if (c === '/') {
      var regexOk = lastSig === '' || '(,=:[!&|?{};+-*%<>~^'.indexOf(lastSig) >= 0 ||
        (lastWord && REGEX_PREV_WORDS[lastWord] === 1);
      if (regexOk) {
        var r = i + 1;
        var inClass = false;
        while (r < n && src[r] !== '\n') {
          if (src[r] === '\\') { r += 2; continue; }
          if (src[r] === '[') { inClass = true; } else if (src[r] === ']') { inClass = false; } else if (src[r] === '/' && !inClass) { break; }
          r++;
        }
        if (r >= n || src[r] !== '/') { problems.push({ kind: 'regex', index: i }); }
        if (!keepStrings) { blank(i + 1, Math.min(r, n)); }
        r++;
        while (r < n && isIdentChar(src[r])) { r++; }
        lastSig = '/'; lastWord = 'regex';
        i = r;
        continue;
      }
    }
    if (c !== ' ' && c !== '\t' && c !== '\n' && c !== '\r') {
      if (isIdentChar(c)) {
        var w = i;
        while (w < n && isIdentChar(src[w])) { w++; }
        lastWord = src.slice(i, w);
        lastSig = src[w - 1];
        i = w;
        continue;
      }
      lastSig = c;
      lastWord = '';
    }
    i++;
  }
  return { code: out.join(''), templates: templates, problems: problems };
}

var KEYWORDS_BEFORE_PAREN = {
  'if': 1, 'for': 1, 'while': 1, 'switch': 1, 'catch': 1, 'function': 1, 'with': 1, 'return': 1,
  'typeof': 1, 'void': 1, 'delete': 1, 'new': 1, 'in': 1, 'do': 1, 'else': 1
};

// Syntax rules apply to every profile; API rules to 'ui' and 'test' only.
var SYNTAX_RULES = [
  { id: 'arrow', re: /=>/g, msg: 'arrow function' },
  { id: 'let', re: /(?<![.\w$])let(?![\w$])/g, msg: '"let"' },
  { id: 'const', re: /(?<![.\w$])const(?![\w$])/g, msg: '"const"' },
  { id: 'class', re: /(?<![.\w$])class(?![\w$])/g, msg: '"class"' },
  { id: 'spread', re: /\.\.\./g, msg: 'spread/rest "..."' },
  { id: 'async', re: /(?<![.\w$])async(?![\w$])/g, msg: '"async"' },
  { id: 'await', re: /(?<![.\w$])await(?![\w$])/g, msg: '"await"' },
  { id: 'forOf', re: /(?<![.\w$])for\s*\(\s*(?:var\s+)?[\w$]+\s+of(?![\w$])/g, msg: 'for...of' },
  { id: 'exponent', re: /\*\*/g, msg: 'exponent operator "**"' },
  { id: 'defaultParam', re: /(?<![\w$])function\s*[\w$]*\s*\([^)]*=/g, msg: 'default parameter' },
  { id: 'shorthandMethod', re: /[{,]\s*([\w$]+)\s*\([^()]*\)\s*\{/g, msg: 'shorthand method', keywordCheck: true }
];

var API_RULES = [
  { id: 'includes', re: /\.includes\s*\(/g, msg: '.includes() (ES2016)' },
  { id: 'objectAssign', re: /(?<![.\w$])Object\s*\.\s*assign(?![\w$])/g, msg: 'Object.assign (ES2015)' },
  { id: 'arrayFrom', re: /(?<![.\w$])Array\s*\.\s*(?:from|of)(?![\w$])/g, msg: 'Array.from/of (ES2015)' },
  { id: 'es6Method', re: /\.(?:padStart|padEnd|trimStart|trimEnd|startsWith|endsWith|repeat|find|findIndex|fill|flat|flatMap|codePointAt)\s*\(/g, msg: 'ES2015+ method' },
  { id: 'es6Static', re: /(?<![.\w$])(?:Object\s*\.\s*(?:entries|values|fromEntries|is|getOwnPropertySymbols)|Number\s*\.\s*(?:isNaN|isFinite|isInteger|parseFloat|parseInt|EPSILON)|Math\s*\.\s*(?:imul|trunc|sign|log2|log10|hypot|cbrt|clz32|fround)|String\s*\.\s*(?:raw|fromCodePoint))(?![\w$])/g, msg: 'ES2015+ static' },
  { id: 'es6Global', re: /(?<![.\w$])(?:new\s+(?:Map|Set|WeakMap|WeakSet|Proxy)|Symbol|Reflect)(?![\w$])/g, msg: 'ES2015+ global' },
  { id: 'promiseStatic', re: /(?<![.\w$])Promise\s*\./g, msg: 'Promise.* outside polyfill_promise.js', except: ['polyfill_promise.js'] },
  { id: 'eval', re: /(?<![.\w$])(?:eval\s*\(|new\s+Function\s*\(|document\s*\.\s*write)/g, msg: 'eval / new Function / document.write' }
];

var UI_ONLY_RULES = [
  { id: 'setInterval', re: /(?:(?<![.\w$])|window\.)setInterval\s*\(/g, msg: 'raw setInterval (use VP_Timers)', except: ['vp_timers.js'] },
  { id: 'rawListener', re: /\.(?:add|remove)EventListener\s*\(/g, msg: 'raw add/removeEventListener (use VP_Dom.on/off)', except: ['vp_dom.js'] },
  { id: 'innerHTML', re: /\.(?:innerHTML|outerHTML)\s*=(?!=)|\.insertAdjacentHTML\s*\(/g, msg: 'HTML injection (use VP_Dom.el / clear)' },
  { id: 'styleAttr', re: /setAttribute\s*\(\s*["']\s*style\s*["']/g, msg: 'style attribute (blocked by CSP; use el.style)', onStrings: true }
];

function lineCol(code, index) {
  var line = 1;
  var last = -1;
  for (var i = 0; i < index; i++) {
    if (code.charCodeAt(i) === 10) { line++; last = i; }
  }
  return { line: line, col: index - last };
}

function lint(src, opts) {
  opts = opts || {};
  var profile = opts.profile || 'ui';
  var file = opts.file || '';
  var res = strip(src);
  var code = res.code;
  var withStrings = null;
  var findings = [];
  var srcLines = src.split('\n');

  function add(rule, index, msg) {
    var lc = lineCol(code, index);
    findings.push({ rule: rule, line: lc.line, col: lc.col, msg: msg, text: (srcLines[lc.line - 1] || '').trim().slice(0, 120) });
  }

  res.templates.forEach(function (idx) { add('backtick', idx, 'template literal (backtick)'); });
  res.problems.forEach(function (p) { add('unterminated', p.index, 'unterminated ' + p.kind); });

  var rules = SYNTAX_RULES.slice();
  if (profile === 'ui' || profile === 'test') { rules = rules.concat(API_RULES); }
  if (profile === 'ui') { rules = rules.concat(UI_ONLY_RULES); }

  rules.forEach(function (rule) {
    if (rule.except && rule.except.indexOf(file) >= 0) { return; }
    rule.re.lastIndex = 0;
    var m;
    var text = code;
    if (rule.onStrings) {
      if (withStrings === null) { withStrings = strip(src, true).code; }
      text = withStrings;
    }
    while ((m = rule.re.exec(text)) !== null) {
      if (rule.keywordCheck && KEYWORDS_BEFORE_PAREN[m[1]] === 1) { continue; }
      add(rule.id, m.index + (rule.keywordCheck ? m[0].indexOf(m[1]) : 0), rule.msg);
    }
  });
  findings.sort(function (a, b) { return a.line - b.line || a.col - b.col; });
  return findings;
}

// IIFE check: after stripping, the file is exactly one statement of the form
// (function (...) { ... })(...);  or  (function (...) { ... }(...));
// Leading 'use strict' is not allowed outside the function (it would apply to the page).
function checkIIFE(src) {
  var code = strip(src).code;
  var start = code.search(/\S/);
  if (start < 0 || code[start] !== '(') { return 'file does not start with "(function"'; }
  if (!/^\(\s*function\s*[\w$]*\s*\([^)]*\)\s*\{/.test(code.slice(start))) {
    return 'file does not start with "(function (...) {"';
  }
  var depth = 0;
  var end = -1;
  for (var i = start; i < code.length; i++) {
    var ch = code[i];
    if (ch === '(' || ch === '{' || ch === '[') { depth++; } else if (ch === ')' || ch === '}' || ch === ']') {
      depth--;
      if (depth === 0) { end = i; break; }
    }
  }
  if (end < 0) { return 'unbalanced brackets'; }
  var rest = code.slice(end + 1);
  var inner = code.slice(start, end + 1);
  if (/^\s*\([^()]*\)\s*;?\s*$/.test(rest)) { return null; }            // (function(){...})(args);
  if (/^\s*;?\s*$/.test(rest) && /\}\s*\([^()]*\)\s*\)$/.test(inner)) { return null; } // (function(){...}(args));
  return 'code after the IIFE: "' + rest.trim().slice(0, 40) + '"';
}

module.exports = { strip: strip, lint: lint, checkIIFE: checkIIFE };
