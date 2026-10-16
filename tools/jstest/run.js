/* run.js - the UI test runner: node tools/jstest/run.js [pattern] [--verbose]
 * (PREPLAN 4.4). Phases: selftest (the checkers catch planted faults), lint (ES5 rules +
 * node --check), contract (one VP_ global per file, IIFE, nothing at load), html, css,
 * i18n, unit tests (gui/ui/tests/*.test.js, filtered by pattern), size report.
 * Exit code 0 only when every phase is clean. No npm packages.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var vm = require('vm');
var zlib = require('zlib');
var childProcess = require('child_process');

var es5lint = require('./es5lint');
var loader = require('./loader');
var harness = require('./harness');
var i18ncheck = require('./i18ncheck');
var checks = require('./checks');
var pseudo = require('./pseudo');

var REPO = path.resolve(__dirname, '..', '..');
var UI = loader.UI_ROOT;
var JS_BUDGET_GZIP = 300 * 1024;

var args = process.argv.slice(2);
var verbose = args.indexOf('--verbose') >= 0;
var pattern = args.filter(function (a) { return a.indexOf('--') !== 0; })[0] || '';

var failures = 0;
var unitPassed = 0;
var unitFailed = 0;

function rel(p) { return path.relative(REPO, p); }
function say(tag, text) { process.stdout.write(('[' + tag + ']          ').slice(0, 11) + text + '\n'); }
function problem(text) { failures++; process.stdout.write('    FAIL ' + text + '\n'); }
function listJs(dir, re) {
  if (!fs.existsSync(dir)) { return []; }
  return fs.readdirSync(dir).filter(function (f) { return re.test(f); }).sort().map(function (f) { return path.join(dir, f); });
}
function kb(n) { return (n / 1024).toFixed(1) + ' KB'; }

// ------------------------------------------------------------------ lint
function lintFiles(files, profile) {
  var bad = 0;
  files.forEach(function (f) {
    var found = es5lint.lint(fs.readFileSync(f, 'utf8'), { profile: profile, file: path.basename(f) });
    found.forEach(function (x) {
      bad++;
      problem(rel(f) + ':' + x.line + ':' + x.col + ' ' + x.rule + ' (' + x.msg + '): ' + x.text);
    });
    var r = childProcess.spawnSync(process.execPath, ['--check', f], { encoding: 'utf8' });
    if (r.status !== 0) {
      bad++;
      problem(rel(f) + ': node --check failed: ' + String(r.stderr).split('\n').slice(0, 5).join(' | '));
    }
  });
  return bad;
}

function phaseLint() {
  var ui = listJs(path.join(UI, 'js'), /\.js$/);
  var tests = listJs(path.join(UI, 'tests'), /\.test\.js$/);
  var tools = listJs(__dirname, /\.js$/).concat(listJs(path.join(UI, 'dev'), /\.js$/));
  var bad = lintFiles(ui, 'ui') + lintFiles(tests, 'test') + lintFiles(tools, 'node');
  say('lint', ui.length + ' UI files, ' + tests.length + ' test files, ' + tools.length + ' tool files: ' + (bad ? bad + ' problem(s)' : 'ES5 clean, node --check ok'));
}

// ------------------------------------------------------------------ contract
function phaseContract() {
  var files = listJs(path.join(UI, 'js'), /\.js$/);
  var seen = {};
  var bad = 0;
  files.forEach(function (f) {
    var base = path.basename(f);
    var iife = es5lint.checkIIFE(fs.readFileSync(f, 'utf8'));
    if (iife) { bad++; problem('js/' + base + ': not one IIFE: ' + iife); }
    var res = loader.contract(base);
    res.errors.forEach(function (e) { bad++; problem('js/' + base + ': ' + e); });
    if (res.global) {
      if (seen[res.global]) { bad++; problem('js/' + base + ': ' + res.global + ' also defined by ' + seen[res.global]); }
      seen[res.global] = base;
    }
  });
  // All files together, in table order: still one global each, nothing running at load.
  var env = loader.load('all');
  if (env.listenerCount() !== 0 || env.clock.pending() !== 0) { bad++; problem('loading every file registers listeners or timers'); }
  say('contract', files.length + ' files: ' + (bad ? bad + ' problem(s)' : 'one VP_ global each (DESIGN 13 names), IIFE, no leaks, idle at load'));
}

// ------------------------------------------------------------------ html + css
function phaseHtml() {
  var res = checks.html(path.join(UI, 'index.html'), path.join(UI, 'js'));
  res.problems.forEach(function (p) { problem('index.html: ' + p); });
  say('html', 'index.html: ' + (res.problems.length ? res.problems.length + ' problem(s)' : 'CSP exact, landmarks, ' + res.scripts + ' scripts in DESIGN 13 order, no inline code'));
}

function phaseCss() {
  var res = checks.css(path.join(UI, 'css'));
  res.problems.forEach(function (p) { problem('css: ' + p); });
  var i = res.info;
  say('css', i.files + ' files, ' + kb(i.bytes) + ' of 60 KB; ' + (i.tokens || 0) + ' tokens x 2 themes as PREDESIGN 2.1; ' + (i.pairs || 0) + ' contrast pairs, tightest text pair ' + (i.worst ? i.worst.toFixed(2) : '?') + ':1' + (res.problems.length ? '; ' + res.problems.length + ' problem(s)' : ''));
}

// ------------------------------------------------------------------ i18n
function phaseI18n() {
  var dynamic = JSON.parse(fs.readFileSync(path.join(__dirname, 'dynamic_keys.json'), 'utf8'));
  var extra = [];
  var env = loader.load('all');
  Object.keys(env.window).forEach(function (name) {
    var mod = env.window[name];
    if (/^VP_/.test(name) && mod && typeof mod.i18nKeys === 'function') { extra = extra.concat(mod.i18nKeys()); }
  });
  var res = i18ncheck.check({
    i18nDir: path.join(UI, 'i18n'),
    jsFiles: listJs(path.join(UI, 'js'), /\.js$/),
    htmlFiles: [path.join(UI, 'index.html')],
    dynamic: dynamic,
    extraUsed: extra
  });
  res.errors.forEach(function (e) { problem('i18n: ' + e); });
  res.warnings.forEach(function (w) { process.stdout.write('    warn ' + w + '\n'); });
  say('i18n', res.stats.keys + ' keys x 2 languages, ' + res.stats.used + ' referenced, ' + res.warnings.length + ' warning(s)' + (res.errors.length ? ', ' + res.errors.length + ' error(s)' : ', complete'));
}

// ------------------------------------------------------------------ unit tests
function testApi() {
  return {
    load: function (files, opts) { return loader.load(files, opts); },
    pseudoLocale: function (dict) { return pseudo.make(dict); },
    readJson: function (relPath) { return JSON.parse(fs.readFileSync(path.join(UI, relPath), 'utf8')); }
  };
}

function runTestFile(file, opts, cb) {
  var suite = new harness.Suite();
  var api = testApi();
  var a = harness.asserts;
  var src = fs.readFileSync(file, 'utf8');
  var wrapped = '(function (describe, it, eq, deepEq, ok, throws, load, pseudoLocale, readJson) {\n' + src + '\n})';
  try {
    var fn = vm.runInThisContext(wrapped, { filename: file, lineOffset: -1 });
    fn(suite.describe, suite.it, a.eq, a.deepEq, a.ok, a.throws, api.load, api.pseudoLocale, api.readJson);
  } catch (e) {
    cb([{ name: path.basename(file) + ' (loading)', ok: false, error: String(e && e.stack) }]);
    return;
  }
  suite.run(opts, cb);
}

function phaseUnit(done) {
  var files = listJs(path.join(UI, 'tests'), /\.test\.js$/).filter(function (f) { return !pattern || path.basename(f).indexOf(pattern) >= 0; });
  var i = 0;
  function next() {
    if (i >= files.length) {
      say('unit', files.length + ' file(s): ' + unitPassed + ' passed, ' + unitFailed + ' failed');
      done();
      return;
    }
    var f = files[i++];
    runTestFile(f, {}, function (results) {
      var pass = results.filter(function (r) { return r.ok; }).length;
      var fail = results.length - pass;
      unitPassed += pass;
      unitFailed += fail;
      if (verbose || fail) {
        results.forEach(function (r) {
          if (r.ok && verbose) { process.stdout.write('    ok   ' + r.name + ' (' + r.ms + ' ms)\n'); }
        });
      }
      results.forEach(function (r) { if (!r.ok) { problem(path.basename(f) + ' > ' + r.name + '\n      ' + r.error); } });
      say('unit', path.basename(f) + ': ' + pass + ' passed' + (fail ? ', ' + fail + ' FAILED' : ''));
      next();
    });
  }
  next();
}

// ------------------------------------------------------------------ sizes
function phaseSize() {
  var files = listJs(path.join(UI, 'js'), /\.js$/);
  var raw = 0;
  var gz = 0;
  files.forEach(function (f) {
    var buf = fs.readFileSync(f);
    raw += buf.length;
    gz += zlib.gzipSync(buf, { level: 9 }).length;
  });
  var all = Buffer.concat(files.map(function (f) { return fs.readFileSync(f); }));
  var gzAll = zlib.gzipSync(all, { level: 9 }).length;
  var cssDir = path.join(UI, 'css');
  var css = listJs(cssDir, /\.css$/).reduce(function (s, f) { return s + fs.statSync(f).size; }, 0);
  var fonts = listJs(path.join(UI, 'fonts'), /\.ttf$/).reduce(function (s, f) { return s + fs.statSync(f).size; }, 0);
  if (gz > JS_BUDGET_GZIP) { problem('JS is ' + kb(gz) + ' gzip, budget ' + kb(JS_BUDGET_GZIP)); }
  if (fonts > 2.6 * 1024 * 1024) { problem('fonts are ' + kb(fonts) + ', budget 2.6 MB'); }
  say('size', 'JS ' + files.length + ' files ' + kb(raw) + ' raw, ' + kb(gz) + ' gzip per file (' + kb(gzAll) + ' as one bundle) of 300 KB; CSS ' + kb(css) + ' of 60 KB; fonts ' + (fonts / 1048576).toFixed(2) + ' MB of 2.6 MB');
}

// ------------------------------------------------------------------ main
function main() {
  process.stdout.write('jstest: vetus poeta UI checks' + (pattern ? ' (unit tests matching "' + pattern + '")' : '') + '\n');
  var selftest = require('./selftest');
  var st = selftest.run({ runTestFile: runTestFile });
  st.problems.forEach(function (p) { problem('selftest: ' + p); });
  st.done(function (extra) {
    extra.forEach(function (p) { problem('selftest: ' + p); });
    say('selftest', st.count + ' planted faults: ' + (st.problems.length + extra.length ? 'some were NOT caught' : 'all caught, clean fixtures pass'));
    phaseLint();
    phaseContract();
    phaseHtml();
    phaseCss();
    phaseI18n();
    phaseUnit(function () {
      phaseSize();
      if (failures) {
        process.stdout.write('jstest: FAIL (' + failures + ' problem(s); unit ' + unitPassed + ' passed, ' + unitFailed + ' failed)\n');
        process.exit(1);
      }
      process.stdout.write('jstest: PASS (unit ' + unitPassed + ' passed, 0 failed; static checks clean)\n');
      process.exit(0);
    });
  });
}

main();
