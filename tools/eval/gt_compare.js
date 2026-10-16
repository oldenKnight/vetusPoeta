/* gt_compare.js - DEV ONLY (task C10, PREPLAN 6.6, DECISIONS D6). Submits cue lines to the Google Translate web
 * page (English -> Latin by default) in polite batches with Node + Playwright driving the pre-installed Chromium,
 * and writes the output, one line per input line, to data/work/gt/<name>.gt.<tl>.txt. That file is third-party
 * output: it stays under data/work/ (gitignored) and is never committed or redistributed.
 *
 *   NODE_PATH=/opt/node-tools/node_modules node tools/eval/gt_compare.js data/work/gt/NAME.src.txt [--name NAME]
 *       [--headful] [--batch 25] [--max-chars 4500] [--limit N] [--sl en] [--tl la] [--out-dir data/work/gt]
 *
 * Input: one plain line per cue (run_eval.py --dump-source). Lines without a letter outside [sound]/(sound)
 * descriptions and music notes are copied, not sent.
 * Method: one browser context, realistic user agent, batches of up to 25 lines separated by blank lines, at most
 * --max-chars characters and --max-url characters of URL-encoded text. Each batch is submitted by loading
 * <base>?sl=..&tl=..&op=translate&text=<encoded batch> (typing into the source box is the fallback when no result
 * appears); the output is split on blank lines and, when the number of parts differs, the batch is redone one line
 * per request. 1.5-3 s jittered pause between requests, no parallelism. The result is found by the lang attribute
 * of the target panel or the polite live region, the source box by ARIA role/label, not by class names; when no
 * result appears the tool stops with a screenshot in data/work/gt/. --base-url points the tool at a local mock page
 * (tests only). CAPTCHA ("unusual traffic") and consent walls stop the run with
 * a message; with --headful the owner can solve them in the window (the tool waits up to 5 minutes).
 */
'use strict';

var fs = require('fs');
var path = require('path');

var ROOT = path.resolve(__dirname, '..', '..');
var CHROME = process.env.VP_CHROMIUM || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
var UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36';

function parseArgs(argv) {
  var a = { input: null, name: null, headful: false, batch: 25, maxChars: 4500, maxUrl: 7000, limit: 0, sl: 'en',
            tl: 'la', outDir: path.join(ROOT, 'data', 'work', 'gt'), baseUrl: 'https://translate.google.com/',
            waitMs: 30000 };
  for (var i = 0; i < argv.length; i++) {
    var k = argv[i];
    if (k === '--headful') a.headful = true;
    else if (k === '--name') a.name = argv[++i];
    else if (k === '--batch') a.batch = Math.max(1, Math.min(25, parseInt(argv[++i], 10) || 25));
    else if (k === '--max-chars') a.maxChars = Math.max(200, Math.min(5000, parseInt(argv[++i], 10) || 4500));
    else if (k === '--max-url') a.maxUrl = Math.max(500, parseInt(argv[++i], 10) || 7000);
    else if (k === '--limit') a.limit = parseInt(argv[++i], 10) || 0;
    else if (k === '--base-url') a.baseUrl = argv[++i];
    else if (k === '--wait-ms') a.waitMs = Math.max(1000, parseInt(argv[++i], 10) || 30000);
    else if (k === '--sl') a.sl = argv[++i];
    else if (k === '--tl') a.tl = argv[++i];
    else if (k === '--out-dir') a.outDir = path.resolve(argv[++i]);
    else if (k.charAt(0) !== '-' && !a.input) a.input = k;
    else { throw new Error('unknown argument ' + k); }
  }
  if (!a.input) throw new Error('usage: gt_compare.js <lines.txt> [--name NAME] [--headful] ...');
  if (!a.name) a.name = path.basename(a.input).split('.')[0];
  var rel = path.relative(ROOT, a.outDir);
  var inRepo = rel === '' || (rel.indexOf('..') !== 0 && !path.isAbsolute(rel));
  if (inRepo && rel.split(path.sep).slice(0, 2).join('/') !== 'data/work') {
    throw new Error('refusing to write third-party output inside the repository outside data/work/: ' + a.outDir);
  }
  return a;
}

// Same rule as evallib.counted(): sound descriptions ([music], (laughs)) and music notes do not count as text.
function hasLetter(s) {
  var rest = s.replace(/\[[^\]]*\]|\([^)]*\)|[\u2669\u266A\u266B\u266C]/g, ' ');
  return /[A-Za-z\u00C0-\u024F\u0370-\u03FF\u0400-\u04FF]/.test(rest);
}

function sleep(ms) { return new Promise(function (res) { setTimeout(res, ms); }); }

var LOCAL = false;   // set for a file: base URL (the tests' mock page): no politeness pause needed offline

function jitter() { return LOCAL ? 0 : 1500 + Math.floor(Math.random() * 1500); }

function log(msg) { process.stderr.write('[gt] ' + msg + '\n'); }

// Batches of at most maxN lines, maxChars characters and maxUrl characters once URL-encoded (blank-line separators
// counted: '\n\n' is 2 characters, '%0A%0A' 6 encoded).
function makeBatches(items, maxN, maxChars, maxUrl) {
  var out = [], cur = [], chars = 0, enc = 0;
  maxUrl = maxUrl || Infinity;
  items.forEach(function (it) {
    var add = it.text.length + 2, addEnc = encodeURIComponent(it.text).length + 6;
    if (cur.length && (cur.length >= maxN || chars + add > maxChars || enc + addEnc > maxUrl)) {
      out.push(cur); cur = []; chars = 0; enc = 0;
    }
    cur.push(it);
    chars += add;
    enc += addEnc;
  });
  if (cur.length) out.push(cur);
  return out;
}

function Session(page, args) {
  this.page = page;
  this.args = args;
  this.requests = 0;
  this.lastResult = '';
}

Session.prototype.shot = function (why) {
  var file = path.join(this.args.outDir, this.args.name + '.failure.png');
  var self = this;
  return this.page.screenshot({ path: file, fullPage: true }).then(function () {
    throw new Error(why + ' (screenshot: ' + path.relative(ROOT, file) + ', url: ' + self.page.url() + ')');
  }, function () { throw new Error(why); });
};

// Consent and CAPTCHA walls: stop, or wait for the owner in a headful window.
Session.prototype.checkWalls = function () {
  var self = this, page = this.page;
  return page.evaluate(function () { return document.body ? document.body.innerText.slice(0, 4000) : ''; })
    .then(function (text) {
      var url = page.url();
      var captcha = /\/sorry\//.test(url) || /unusual traffic|not a robot|captcha/i.test(text);
      var consent = /consent\.google\./.test(url) || /Before you continue to Google|Antes de ir a Google/i.test(text);
      if (!captcha && !consent) return null;
      var what = captcha ? 'CAPTCHA (unusual traffic page)' : 'consent wall';
      if (!self.args.headful) {
        return self.shot('stopped at a ' + what + ': rerun with --headful and complete it in the window, or try later');
      }
      log(what + ' shown: complete it in the browser window; waiting up to 5 minutes');
      return page.waitForURL(/translate\.google\.[a-z.]+\/(\?|$)/, { timeout: 300000 })
        .then(function () { return page.waitForLoadState('domcontentloaded'); })
        .then(function () { return null; }, function () { return self.shot('the ' + what + ' was not completed'); });
    });
};

function pageUrl(args, text) {
  var u = args.baseUrl + (args.baseUrl.indexOf('?') >= 0 ? '&' : '?') + 'sl=' + encodeURIComponent(args.sl) +
          '&tl=' + encodeURIComponent(args.tl) + '&op=translate';
  return text === undefined ? u : u + '&text=' + encodeURIComponent(text);
}

Session.prototype.open = function () {
  var self = this;
  return this.page.goto(pageUrl(this.args), { waitUntil: 'domcontentloaded', timeout: 60000 })
    .then(function () { return self.checkWalls(); });
};

// The source box: the textarea labelled "Source text" (seen in the served page on 2026-10-06:
// <textarea aria-label="Source text" aria-autocomplete="list" ...>), else a combobox with that name, else the first
// labelled textarea.
Session.prototype.sourceBox = function () {
  var page = this.page, self = this;
  var candidates = [
    page.getByRole('textbox', { name: /source text|texto de origen/i }),
    page.getByRole('combobox', { name: /source text|texto de origen/i }),
    page.locator('textarea[aria-label]').first()
  ];
  function tryAt(i) {
    if (i >= candidates.length) return self.shot('source text box not found by ARIA role/label');
    var loc = candidates[i].first();
    return loc.waitFor({ state: 'visible', timeout: i === 0 ? 20000 : 3000 })
      .then(function () { self.box = loc; return loc; }, function () { return tryAt(i + 1); });
  }
  return tryAt(0);
};

// The result: the element marked with the target language (lang="la") that is not the source box (the served page
// wraps the source panel in <span lang="en">, the result panel is expected to mirror it); as a fallback the polite
// live region the page uses to announce the translation. Placeholders ("Translation", "Translating...") are no
// result.
Session.prototype.readResult = function () {
  var tl = this.args.tl;
  return this.page.evaluate(function (tl) {
    function visible(el) { var r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0; }
    function placeholder(t) { return /^\s*(translation|translating\W*|traducci\u00f3n|traduciendo\W*)\s*$/i.test(t); }
    var nodes = Array.prototype.slice.call(document.querySelectorAll('[lang="' + tl + '"]'));
    var best = null;
    nodes.forEach(function (n) {
      if (n === document.documentElement || n.tagName === 'TEXTAREA' || !visible(n)) return;
      if (n.querySelector('textarea')) return;
      var t = n.innerText || '';
      if (t.trim() && !placeholder(t) && (!best || (best.contains(n) ? false : t.length > (best.innerText || '').length))) best = n;
    });
    if (best) return { how: 'lang=' + tl, text: best.innerText };
    var live = Array.prototype.slice.call(document.querySelectorAll('[aria-live="polite"]')).filter(function (n) {
      return visible(n) && (n.innerText || '').trim() && !placeholder(n.innerText);
    });
    if (live.length) return { how: 'aria-live', text: live[live.length - 1].innerText };
    return null;
  }, tl);
};

// Waits until a result is present and unchanged for 1.2 s (the page streams long results); resolves with the text,
// or null after --wait-ms. `stale` is a result that must not be taken (the previous one, typing path).
Session.prototype.waitResult = function (stale) {
  var self = this, t0 = Date.now();
  function poll() {
    return self.readResult().then(function (r) {
      if (r && r.text.trim() && r.text !== stale) {
        return sleep(1200).then(function () { return self.readResult(); }).then(function (r2) {
          if (r2 && r2.text === r.text) { self.how = r.how; return r.text; }
          return poll();
        });
      }
      if (Date.now() - t0 > self.args.waitMs) return null;
      return sleep(400).then(poll);
    });
  }
  return poll();
};

// Primary path: load the page with the batch in the URL (the text arrives whole, the page translates on load).
// Fallback: type the text into the source box of the loaded page.
Session.prototype.translate = function (text) {
  var self = this, page = this.page;
  this.requests++;
  return page.goto(pageUrl(this.args, text), { waitUntil: 'domcontentloaded', timeout: 60000 })
    .then(function () { return self.checkWalls(); })
    .then(function () { return self.waitResult(null); })
    .then(function (out) {
      if (out !== null) { self.path = 'url'; return out; }
      log('no result from the URL path after ' + self.args.waitMs + ' ms, typing into the source box');
      return self.typeText(text);
    });
};

Session.prototype.typeText = function (text) {
  var self = this, page = this.page, before = null;
  return this.readResult()
    .then(function (r) { before = r ? r.text : null; return self.sourceBox(); })
    .then(function (box) { return box.click().then(function () { return box.fill(''); }); })
    .then(function () { return page.keyboard.insertText(text); })   // one input event with the whole text
    .then(function () { return sleep(300); })
    .then(function () { return self.box.inputValue(); })
    .then(function (v) {
      if (v.replace(/\s+/g, ' ').trim() !== text.replace(/\s+/g, ' ').trim()) {
        return self.shot('the source box does not hold the whole batch after typing');
      }
      return self.waitResult(before);
    })
    .then(function (out) {
      if (out !== null) { self.path = 'typed'; return out; }
      return self.checkWalls().then(function () {
        return self.shot('no translation result found after ' + (2 * self.args.waitMs / 1000) + ' s (URL and typing)');
      });
    });
};

function splitParts(out) {
  return out.replace(/\r/g, '').split(/\n\s*\n/).map(function (s) { return s.replace(/\s+/g, ' ').trim(); })
    .filter(function (s) { return s.length > 0; });
}

function main() {
  var args = parseArgs(process.argv.slice(2));
  var pw;
  try { pw = require('playwright'); } catch (e) {
    try { pw = require('/opt/node-tools/node_modules/playwright'); } catch (e2) {
      throw new Error('Playwright not found: set NODE_PATH=/opt/node-tools/node_modules');
    }
  }
  LOCAL = /^file:/.test(args.baseUrl);
  fs.mkdirSync(args.outDir, { recursive: true });
  var lines = fs.readFileSync(args.input, 'utf8').replace(/\r/g, '').split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  if (args.limit > 0) lines = lines.slice(0, args.limit);
  var result = lines.map(function (l) { return hasLetter(l) ? null : l; });
  var items = [];
  lines.forEach(function (l, i) { if (hasLetter(l)) items.push({ i: i, text: l.replace(/\s+/g, ' ').trim() }); });
  var batches = makeBatches(items, args.batch, args.maxChars, args.maxUrl);
  var stats = { lines: lines.length, sent: items.length, batches: batches.length, fallbacks: 0, requests: 0, typed: 0 };
  log(items.length + ' lines to translate in ' + batches.length + ' batches');

  var launch = { executablePath: CHROME, headless: !args.headful };
  var proxy = process.env.HTTPS_PROXY || process.env.https_proxy;
  if (proxy) launch.proxy = { server: proxy };
  var browser, session;
  return pw.chromium.launch(launch).then(function (b) {
    browser = b;
    return browser.newContext({ userAgent: UA, locale: 'en-US', viewport: { width: 1280, height: 900 } });
  }).then(function (ctx) { return ctx.newPage(); })
    .then(function (page) { session = new Session(page, args); return session.open(); })
    .then(function () {
      var chain = Promise.resolve();
      batches.forEach(function (batch, bi) {
        chain = chain.then(function () {
          var text = batch.map(function (it) { return it.text; }).join('\n\n');
          var wait = bi === 0 ? Promise.resolve() : sleep(jitter());
          return wait.then(function () { return session.translate(text); }).then(function (out) {
            var parts = splitParts(out);
            if (parts.length === batch.length) {
              batch.forEach(function (it, k) { result[it.i] = parts[k]; });
              log('batch ' + (bi + 1) + '/' + batches.length + ': ' + batch.length + ' lines (' + session.path +
                  ', ' + session.how + ')');
              if (session.path === 'typed') stats.typed++;
              return null;
            }
            stats.fallbacks++;
            log('batch ' + (bi + 1) + ': ' + parts.length + ' parts for ' + batch.length + ' lines, one per request');
            var c = sleep(jitter());
            batch.forEach(function (it) {
              c = c.then(function () { return session.translate(it.text); })
                .then(function (o) {
                  result[it.i] = o.replace(/\s+/g, ' ').trim();
                  if (session.path === 'typed') stats.typed++;
                  return sleep(jitter());
                });
            });
            return c;
          });
        });
      });
      return chain;
    })
    .then(function () {
      stats.requests = session.requests;
      var out = path.join(args.outDir, args.name + '.gt.' + args.tl + '.txt');
      fs.writeFileSync(out, result.map(function (r) { return r === null ? '' : r; }).join('\n') + '\n', 'utf8');
      var meta = { tool: 'tools/eval/gt_compare.js', at: new Date().toISOString(), sl: args.sl, tl: args.tl,
                   input: path.basename(args.input), output: path.basename(out), userAgent: UA, resultLocator: session.how,
                   stats: stats, note: 'third-party output; never commit or redistribute' };
      fs.writeFileSync(path.join(args.outDir, args.name + '.gt.meta.json'), JSON.stringify(meta, null, 2) + '\n');
      log('wrote ' + path.relative(ROOT, out) + ' (' + stats.requests + ' requests, ' + stats.fallbacks + ' fallbacks)');
      return browser.close();
    })
    .catch(function (e) {
      log('STOPPED: ' + (e && e.message ? e.message : e));
      process.exitCode = 1;
      return browser ? browser.close() : null;
    });
}

if (require.main === module) {
  try { main(); } catch (e) { log('STOPPED: ' + e.message); process.exitCode = 2; }
}

module.exports = { makeBatches: makeBatches, splitParts: splitParts, hasLetter: hasLetter, pageUrl: pageUrl };
