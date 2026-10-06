/* selftest.js - proves the checkers catch what they claim to (PREPLAN 4.4). Every fixture
 * under selftest/ is deliberately bad (or deliberately tricky but clean) and says what it
 * expects in its first line:
 *   es5/*.js        // expect: rule rule ...     (empty list = must be clean)
 *   contract/*.js   // expect-contract: text      (substring of the error; "none" = clean)
 *   i18n/<case>/    expect.txt: one substring per line (empty = clean); en-US.json, es-MX.json,
 *                   optional ui.js and ui.html
 *   unit/*.test.js  // expect-unit: <passed> <failed>
 * run({runTestFile}) -> {count, problems, done(cb(extraProblems))}
 */
'use strict';

var fs = require('fs');
var path = require('path');
var es5lint = require('./es5lint');
var loader = require('./loader');
var i18ncheck = require('./i18ncheck');

var DIR = path.join(__dirname, 'selftest');

function files(sub, re) {
  var d = path.join(DIR, sub);
  if (!fs.existsSync(d)) { return []; }
  return fs.readdirSync(d).filter(function (f) { return re.test(f); }).sort().map(function (f) { return path.join(d, f); });
}

function firstLine(file) { return fs.readFileSync(file, 'utf8').split('\n')[0]; }

function run(opts) {
  var problems = [];
  var count = 0;

  files('es5', /\.js$/).forEach(function (f) {
    count++;
    var m = /\/\/ expect:(.*)$/.exec(firstLine(f));
    if (!m) { problems.push(path.basename(f) + ': no "// expect:" line'); return; }
    var want = m[1].trim() ? m[1].trim().split(/\s+/).sort() : [];
    var src = fs.readFileSync(f, 'utf8');
    var profile = /\.node\.js$/.test(f) ? 'node' : 'ui';
    var got = es5lint.lint(src, { profile: profile, file: path.basename(f) }).map(function (x) { return x.rule; });
    var uniq = got.filter(function (r, i) { return got.indexOf(r) === i; }).sort();
    if (uniq.join(' ') !== want.join(' ')) { problems.push('es5/' + path.basename(f) + ': expected [' + want.join(' ') + '], lint found [' + uniq.join(' ') + ']'); }
  });

  files('contract', /\.js$/).forEach(function (f) {
    count++;
    var m = /\/\/ expect-contract:(.*)$/.exec(firstLine(f));
    var want = m ? m[1].trim() : '';
    var iife = es5lint.checkIIFE(fs.readFileSync(f, 'utf8'));
    var res = loader.contract(f, { anyName: true });
    var errs = res.errors.concat(iife ? ['IIFE: ' + iife] : []);
    if (want === 'none') {
      if (errs.length) { problems.push('contract/' + path.basename(f) + ': expected clean, got: ' + errs.join('; ')); }
    } else if (!errs.some(function (e) { return e.indexOf(want) >= 0; })) {
      problems.push('contract/' + path.basename(f) + ': expected an error containing "' + want + '", got: [' + errs.join('; ') + ']');
    }
  });

  var i18nDir = path.join(DIR, 'i18n');
  if (fs.existsSync(i18nDir)) {
    fs.readdirSync(i18nDir).sort().forEach(function (c) {
      var d = path.join(i18nDir, c);
      if (!fs.statSync(d).isDirectory()) { return; }
      count++;
      var want = fs.readFileSync(path.join(d, 'expect.txt'), 'utf8').split('\n').map(function (s) { return s.trim(); }).filter(Boolean);
      var res = i18ncheck.check({
        i18nDir: d,
        jsFiles: fs.existsSync(path.join(d, 'ui.js')) ? [path.join(d, 'ui.js')] : [],
        htmlFiles: fs.existsSync(path.join(d, 'ui.html')) ? [path.join(d, 'ui.html')] : [],
        dynamic: { errorCodes: [], families: {} }
      });
      if (!want.length && res.errors.length) { problems.push('i18n/' + c + ': expected clean, got: ' + res.errors.join('; ')); }
      want.forEach(function (w) {
        if (!res.errors.some(function (e) { return e.indexOf(w) >= 0; })) { problems.push('i18n/' + c + ': expected an error containing "' + w + '", got: [' + res.errors.join('; ') + ']'); }
      });
    });
  }

  var unitFiles = files('unit', /\.test\.js$/);
  count += unitFiles.length;

  return {
    count: count,
    problems: problems,
    done: function (cb) {
      var extra = [];
      var i = 0;
      function next() {
        if (i >= unitFiles.length) { cb(extra); return; }
        var f = unitFiles[i++];
        var m = /\/\/ expect-unit:\s*(\d+)\s+(\d+)/.exec(firstLine(f));
        opts.runTestFile(f, { timeoutMs: 150 }, function (results) {
          var pass = results.filter(function (r) { return r.ok; }).length;
          var fail = results.length - pass;
          if (!m || Number(m[1]) !== pass || Number(m[2]) !== fail) {
            extra.push('unit/' + path.basename(f) + ': expected ' + (m ? m[1] + ' passed ' + m[2] + ' failed' : '?') + ', harness reported ' + pass + ' passed ' + fail + ' failed');
          }
          next();
        });
      }
      next();
    }
  };
}

module.exports = { run: run };
