/* gt_compare.js - DEV ONLY (task C10, PREPLAN 6.6, DECISIONS D6). Submits cue lines to the Google Translate web
 * page (English -> Latin by default) in polite batches with Node + Playwright driving the pre-installed Chromium,
 * and writes the output, one line per input line, to data/work/gt/<name>.gt.<tl>.txt. That file is third-party
 * output: it stays under data/work/ (gitignored) and is never committed or redistributed.
 *
 *   NODE_PATH=/opt/node-tools/node_modules node tools/eval/gt_compare.js data/work/gt/NAME.src.txt [--name NAME]
 *       [--headful] [--batch 25] [--max-chars 4500] [--limit N] [--sl en] [--tl la] [--out-dir data/work/gt]
 *
 * Input: one plain line per cue (run_eval.py --dump-source). Lines without a letter are copied, not sent.
 * Method: one browser context, realistic user agent, batches of up to 25 lines separated by blank lines and at most
 * --max-chars characters; the output is split on blank lines and, when the number of parts differs, the batch is
 * redone one line per request. 1.5-3 s jittered pause between requests, no parallelism. The source box and the
 * result are found by ARIA role/label and the lang attribute, not by class names; when they cannot be found the
 * tool stops with a screenshot in data/work/gt/. CAPTCHA ("unusual traffic") and consent walls stop the run with
 * a message; with --headful the owner can solve them in the window (the tool waits up to 5 minutes).
 */
'use strict';

var fs = require('fs');
var path = require('path');

var ROOT = path.resolve(__dirname, '..', '..');
var CHROME = process.env.VP_CHROMIUM || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
var UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36';

function parseArgs(argv) {
  var a = { input: null, name: null, headful: false, batch: 25, maxChars: 4500, limit: 0, sl: 'en', tl: 'la',
            outDir: path.join(ROOT, 'data', 'work', 'gt') };
  for (var i = 0; i < argv.length; i++) {
    var k = argv[i];
    if (k === '--headful') a.headful = true;
    else if (k === '--name') a.name = argv[++i];
    else if (k === '--batch') a.batch = Math.max(1, Math.min(25, parseInt(argv[++i], 10) || 25));
    else if (k === '--max-chars') a.maxChars = Math.max(200, Math.min(5000, parseInt(argv[++i], 10) || 4500));
    else if (k === '--limit') a.limit = parseInt(argv[++i], 10) || 0;
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

function hasLetter(s) { return /[A-Za-zÀ-ɏͰ-ϿЀ-ӿ]/.test(s); }

function sleep(ms) { return new Promise(function (res) { setTimeout(res, ms); }); }

function jitter() { return 1500 + Math.floor(Math.random() * 1500); }

function log(msg) { process.stderr.write('[gt] ' + msg + '\n'); }

function makeBatches(items, maxN, maxChars) {
  var out = [], cur = [], chars = 0;
  items.forEach(function (it) {
    var add = it.text.length + 2;
    if (cur.length && (cur.length >= maxN || chars + add > maxChars)) { out.push(cur); cur = []; chars = 0; }
    cur.push(it);
    chars += add;
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

Session.prototype.open = function () {
  var a = this.args, self = this;
  var url = 'https://translate.google.com/?sl=' + encodeURIComponent(a.sl) + '&tl=' + encodeURIComponent(a.tl) +
            '&op=translate';
  return this.page.goto(url, { waitUntil: 'domcontentloaded', timeout: 60000 })
    .then(function () { return self.checkWalls(); })
    .then(function () { return self.sourceBox(); });
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
// wraps the source panel in <span lang="en">, the result panel is expected to mirror it; unverified in a browser
// here); as a fallback the polite live region the page uses to announce the translation.
Session.prototype.readResult = function () {
  var tl = this.args.tl;
  return this.page.evaluate(function (tl) {
    function visible(el) { var r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0; }
    var nodes = Array.prototype.slice.call(document.querySelectorAll('[lang="' + tl + '"]'));
    var best = null;
    nodes.forEach(function (n) {
      if (n === document.documentElement || n.tagName === 'TEXTAREA' || !visible(n)) return;
      if (n.querySelector('textarea')) return;
      var t = n.innerText || '';
      if (t.trim() && (!best || (best.contains(n) ? false : t.length > (best.innerText || '').length))) best = n;
    });
    if (best) return { how: 'lang=' + tl, text: best.innerText };
    var live = Array.prototype.slice.call(document.querySelectorAll('[aria-live="polite"]')).filter(function (n) {
      return visible(n) && (n.innerText || '').trim();
    });
    if (live.length) return { how: 'aria-live', text: live[live.length - 1].innerText };
    return null;
  }, tl);
};

Session.prototype.translate = function (text) {
  var self = this, page = this.page;
  this.requests++;
  var t0 = Date.now();
  return this.box.fill('')
    .then(function () { return sleep(300); })
    .then(function () { return self.box.fill(text); })
    .then(function poll() {
      return self.readResult().then(function (r) {
        var elapsed = Date.now() - t0;
        if (r && r.text.trim() && r.text !== self.lastResult) {
          // wait until the text is stable for 1.2 s (the page streams long results)
          return sleep(1200).then(function () { return self.readResult(); }).then(function (r2) {
            if (r2 && r2.text === r.text) { self.lastResult = r.text; self.how = r.how; return r.text; }
            return poll();
          });
        }
        if (elapsed > 30000) {
          return self.checkWalls().then(function () { return self.shot('no translation result found after 30 s'); });
        }
        return sleep(400).then(poll);
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
  fs.mkdirSync(args.outDir, { recursive: true });
  var lines = fs.readFileSync(args.input, 'utf8').replace(/\r/g, '').split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  if (args.limit > 0) lines = lines.slice(0, args.limit);
  var result = lines.map(function (l) { return hasLetter(l) ? null : l; });
  var items = [];
  lines.forEach(function (l, i) { if (hasLetter(l)) items.push({ i: i, text: l.replace(/\s+/g, ' ').trim() }); });
  var batches = makeBatches(items, args.batch, args.maxChars);
  var stats = { lines: lines.length, sent: items.length, batches: batches.length, fallbacks: 0, requests: 0 };
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
          return session.translate(text).then(function (out) {
            var parts = splitParts(out);
            if (parts.length === batch.length) {
              batch.forEach(function (it, k) { result[it.i] = parts[k]; });
              log('batch ' + (bi + 1) + '/' + batches.length + ': ' + batch.length + ' lines (' + session.how + ')');
              return sleep(jitter());
            }
            stats.fallbacks++;
            log('batch ' + (bi + 1) + ': ' + parts.length + ' parts for ' + batch.length + ' lines, one per request');
            var c = sleep(jitter());
            batch.forEach(function (it) {
              c = c.then(function () { return session.translate(it.text); })
                .then(function (o) { result[it.i] = o.replace(/\s+/g, ' ').trim(); return sleep(jitter()); });
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

module.exports = { makeBatches: makeBatches, splitParts: splitParts, hasLetter: hasLetter };
