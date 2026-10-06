/* smoke_real.js - DEV ONLY browser smoke test of the UI against the REAL engine (vpengine serve), through
 * engine_bridge_shim.js. Needs Playwright + Chromium (like smoke.js) and the engine with its data; prints SKIP and
 * exits 0 when any of them is missing.
 *   node gui/ui/dev/smoke_real.js [--engine build/engine/cli/vpengine] [--lexicons data/work]
 * Steps: boot (bridge kind "webview"), start screen with the dictionary line, "Try the sample" opens
 * <dataDir>/samples/sample.en.srt (installed by the engine), Translate runs to the end, the Word tab with the
 * inspector on the first word ("why this word" and the paradigm table), the Words tab, the Export dialog with its
 * preview. Screenshots: gui/ui/dev/out/real-*.png (gitignored). It prints what the engine sent where the panels
 * read it, so differences from the mock run (smoke.js) are visible.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var shim = require('./engine_bridge_shim');

var OUT = path.join(__dirname, 'out');
var REPO = path.resolve(__dirname, '..', '..', '..');
var problems = [];

function note(s) { process.stdout.write('  ' + s + '\n'); }
function check(cond, what) {
  if (cond) { process.stdout.write('  ok   ' + what + '\n'); } else {
    problems.push(what);
    process.stdout.write('  FAIL ' + what + '\n');
  }
}
function skip(why) {
  process.stdout.write('smoke_real: SKIP (' + why + ')\n');
  process.exit(0);
}

function loadPlaywright() {
  var tries = [];
  if (process.env.NODE_PATH) { tries = tries.concat(process.env.NODE_PATH.split(path.delimiter).map(function (p) { return path.join(p, 'playwright'); })); }
  tries.push('playwright', '/opt/node-tools/node_modules/playwright');
  for (var i = 0; i < tries.length; i++) {
    try { return require(tries[i]); } catch (e) { /* next */ }
  }
  return null;
}

function findChromium() {
  if (process.env.VP_CHROMIUM && fs.existsSync(process.env.VP_CHROMIUM)) { return process.env.VP_CHROMIUM; }
  var base = process.env.PLAYWRIGHT_BROWSERS_PATH || '/opt/pw-browsers';
  if (!fs.existsSync(base)) { return null; }
  var dirs = fs.readdirSync(base).filter(function (d) { return /^chromium-\d+$/.test(d); }).sort().reverse();
  for (var i = 0; i < dirs.length; i++) {
    var exe = path.join(base, dirs[i], 'chrome-linux', 'chrome');
    if (fs.existsSync(exe)) { return exe; }
  }
  return null;
}

function arg(name, def) {
  var i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
}

function main() {
  var engine = path.resolve(arg('--engine', process.env.VP_ENGINE || path.join(REPO, 'build', 'engine', 'cli', 'vpengine')));
  var lexicons = path.resolve(arg('--lexicons', path.join(REPO, 'data', 'work')));
  if (!fs.existsSync(engine)) { skip('no engine at ' + engine); }
  if (!fs.existsSync(path.join(lexicons, 'latin.vpl'))) { skip('no latin.vpl in ' + lexicons); }
  var pw = loadPlaywright();
  if (!pw) { skip('Playwright not found'); }
  var exe = findChromium();
  if (!fs.existsSync(OUT)) { fs.mkdirSync(OUT, { recursive: true }); }
  var data = fs.mkdtempSync(path.join(require('os').tmpdir(), 'vp-smoke-real-'));
  // the first-run tour is marked seen (smoke.js runs it); VP_REAL_TOUR=1 keeps it to see it start on the real engine
  if (process.env.VP_REAL_TOUR !== '1') { fs.writeFileSync(path.join(data, 'settings.json'), JSON.stringify({ tourSeenVersion: '1' })); }
  var srv = null;
  var browser = null;
  var page = null;
  var errors = [];

  function shot(name) {
    return page.waitForTimeout(300).then(function () { return page.screenshot({ path: path.join(OUT, name) }); }).then(function () { note('screenshot dev/out/' + name); });
  }

  new Promise(function (resolve, reject) {
    shim.start({ engine: engine, lexicons: lexicons, data: data }, function (err, s) { if (err) { reject(err); } else { resolve(s); } });
  }).then(function (s) {
    srv = s;
    return pw.chromium.launch({ executablePath: exe || undefined, headless: true }).then(null, function (e) {
      srv.close(function () {});
      skip('cannot start Chromium: ' + String(e && e.message).split('\n')[0]);
    });
  }).then(function (b) {
    browser = b;
    return browser.newContext({ viewport: { width: 1280, height: 800 }, colorScheme: 'light', reducedMotion: 'reduce' });
  }).then(function (ctx) {
    return ctx.newPage();
  }).then(function (p) {
    page = p;
    page.on('console', function (m) { if (m.type() === 'error') { errors.push('console: ' + m.text()); } });
    page.on('pageerror', function (e) { errors.push('pageerror: ' + String(e && e.message)); });
    page.on('requestfailed', function (r) { if (r.url().indexOf('/__vp/events') < 0) { errors.push('request failed: ' + r.url()); } });
    page.on('response', function (r) { if (r.status() >= 400) { errors.push('HTTP ' + r.status() + ': ' + r.url()); } });
    process.stdout.write('smoke_real: ' + srv.url + 'index.html?debug=1, engine ' + engine + ', data ' + data + '\n');
    return page.goto(srv.url + 'index.html?debug=1');
  }).then(function () {
    return page.waitForFunction(function () { return !!(window.VP_App && window.VP_App.ready()); }, null, { timeout: 30000, polling: 100 });
  }).then(function () {
    return page.evaluate(function () {
      return { kind: window.VP_Bridge.kind(), line: document.getElementById('vp-start-status').textContent, status: document.getElementById('vp-status').textContent };
    });
  }).then(function (r) {
    check(r.kind === 'webview', 'bridge uses the (shimmed) WebView2 transport (' + r.kind + ')');
    check(/65,315|dictionar/i.test(r.line), 'start screen engine line: ' + r.line);
    note('status bar: ' + r.status);
    return page.waitForTimeout(500);
  }).then(function () {
    // a fresh data folder: the first-run tour starts by itself; it is dismissed (smoke.js covers it)
    return page.evaluate(function () {
      var was = window.VP_Tour.isActive();
      if (was) { window.VP_Tour.skip(); }
      return was;
    });
  }).then(function (was) {
    note('first-run tour ' + (was ? 'was showing; skipped' : 'not shown'));
    return shot('real-start-light-en.png');
  }).then(function () {
    return page.click('[data-start-action="sample"]');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_Store.cueCount() === 12; }, null, { timeout: 20000, polling: 100 });
  }).then(function () {
    return page.evaluate(function () { return { name: document.querySelector('.vp-ws-name').textContent, src: document.getElementById('vp-src-text').textContent }; });
  }).then(function (r) {
    check(r.name === 'sample.en.srt' && r.src === 'The girl sees the rose.', 'the sample from <dataDir>/samples opens (' + r.name + ': ' + r.src + ')');
    return page.click('#vp-translate');
  }).then(function () {
    return page.waitForFunction(function () {
      var c = document.querySelector('.vp-ws-counts');
      return !window.VP_Store.get('job') && c && c.textContent.indexOf('12 / 12 translated') === 0;
    }, null, { timeout: 60000, polling: 100 });
  }).then(function () {
    return page.evaluate(function () {
      var L = window.VP_CueList;
      var sel = document.getElementById('vp-cl-filter');
      return { filter: sel.options[sel.selectedIndex].textContent, counts: document.querySelector('.vp-ws-counts').textContent, target: document.getElementById('vp-target-view') ? document.getElementById('vp-target-view').textContent : '', selected: L.selected(), chips: document.querySelectorAll('.vp-word').length };
    });
  }).then(function (r) {
    check(r.counts.indexOf('12 / 12 translated') === 0, 'translation finished: ' + r.counts + '; filter "' + r.filter + '"');
    check(r.chips > 0, 'target word chips: ' + r.chips);
    return page.evaluate(function () { window.VP_Toast.clearAll(); });
  }).then(function () {
    return shot('real-workspace-light-en.png');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_CueList.select(0);
      return new Promise(function (resolve) { setTimeout(resolve, 600); }).then(function () {
        window.VP_Panes.openWord(0);
        return new Promise(function (resolve) { setTimeout(resolve, 800); });
      }).then(function () {
        window.VP_Inspector.toggleWhy(true);
        window.VP_Inspector.toggleForms(true);
        return new Promise(function (resolve) { setTimeout(resolve, 800); });
      }).then(function () {
        var st = window.VP_Inspector.state();
        var q = function (s) { return document.querySelectorAll(s).length; };
        var body = document.getElementById('vp-panel-body');
        return {
          tab: window.VP_Workspace.tab(), word: st.word, loaded: st.loaded, blocks: q('.vp-why-block'), table: !!document.querySelector('.vp-paradigm'),
          used: q('.vp-par-used'), cands: q('.vp-why-cand'), chosen: q('.vp-why-chosen'), evYes: q('.vp-ev-yes'), evOff: q('.vp-ev-off'), evNone: q('.vp-ev-none'),
          text: body ? body.textContent.replace(/\s+/g, ' ').slice(0, 600) : ''
        };
      });
    });
  }).then(function (r) {
    check(r.tab === 'word' && !!r.word && r.loaded, 'Word tab: inspector on "' + r.word + '"');
    check(r.blocks === 4, 'four "why this word" blocks');
    check(r.table && r.used >= 1, 'paradigm table with the used cell highlighted (' + r.used + ')');
    check(r.cands >= 1 && r.chosen === 1, 'candidates listed (' + r.cands + '), one chosen');
    note('evidence rows: yes ' + r.evYes + ', off ' + r.evOff + ', none ' + r.evNone);
    note('Word tab text: ' + r.text);
    return shot('real-panel-word-light-en.png');
  }).then(function () {
    return page.click('#vp-ptab-words');
  }).then(function () {
    return page.waitForTimeout(800);
  }).then(function () {
    return page.evaluate(function () { return document.getElementById('vp-panel-body').textContent.replace(/\s+/g, ' ').slice(0, 300); });
  }).then(function (t) {
    check(t.length > 20, 'Words tab: ' + t);
    return shot('real-panel-words-light-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_Workspace.setTab('word'); });
  }).then(function () {
    return page.keyboard.press('Control+Shift+E');
  }).then(function () {
    return page.waitForTimeout(1200);
  }).then(function () {
    return page.evaluate(function () {
      var lines = document.querySelectorAll('.vp-exp-preview .vp-preview-line');
      var out = [];
      for (var i = 0; i < lines.length; i++) { out.push(lines[i].textContent); }
      return { open: window.VP_Export.isOpen(), lines: out };
    });
  }).then(function (r) {
    check(r.open && r.lines.length >= 3, 'Export dialog with a preview (' + r.lines.join(' | ') + ')');
    return shot('real-dialog-export-light-en.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    // Known UI finding (reported to the UI owner, not an engine problem): with the WebView2 transport VP_App
    // initialises the bridge after the Start screen is mounted, so its 'message' listener is counted as a Start
    // screen leak by the debug router check. The mock transport registers no VP_Dom listener, so smoke.js never sees it.
    var known = /router\.leak \{"screen":"start","diff":\{"listeners":\{"before":1,"after":2\}\}\}/;
    var findings = errors.filter(function (e) { return known.test(e); });
    var rest = errors.filter(function (e) { return !known.test(e); });
    if (findings.length) { note('UI finding: ' + findings[0] + ' (bridge listener added after the Start screen mounted)'); }
    check(rest.length === 0, 'no other console errors or failed requests' + (rest.length ? ': ' + rest.slice(0, 5).join(' / ') : ''));
    check(!srv.exited(), 'the engine is still running');
    note('shell commands answered by the shim: ' + srv.shellCalls().map(function (c) { return c.cmd; }).join(', '));
  }).then(null, function (e) {
    problems.push('exception: ' + String(e && (e.stack || e.message)));
    process.stdout.write('  FAIL ' + String(e && (e.stack || e.message)) + '\n');
  }).then(function () {
    return browser ? browser.close() : null;
  }).then(function () {
    return new Promise(function (resolve) { if (srv) { srv.close(resolve); } else { resolve(); } });
  }).then(function () {
    process.stdout.write('smoke_real: ' + (problems.length ? problems.length + ' problem(s)' : 'PASS') + '\n');
    process.exit(problems.length ? 1 : 0);
  });
}

main();
