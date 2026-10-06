/* smoke.js - DEV ONLY browser smoke test (PREDESIGN 6.2/6.3). Needs Playwright and a
 * Chromium; prints SKIP and exits 0 when either is missing.
 *   node gui/ui/dev/smoke.js     (NODE_PATH or /opt/node-tools/node_modules for Playwright,
 *                                 PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers for Chromium)
 * Checks: no console errors, CSP violations or failed requests; boot with the mock engine;
 * 50 mounts/destroys of the start screen return VP_Debug.stats() to baseline; Gentium Plus
 * renders the Latin (macrons, breves) and polytonic Greek sample (document.fonts.check plus
 * Chromium's own platform-font report); auto dark follows the system; DOM and heap budgets;
 * shortcut dialog and tour by keyboard. Screenshots go to gui/ui/dev/out/ (gitignored).
 */
'use strict';

var fs = require('fs');
var path = require('path');
var serve = require('./serve');

var OUT = path.join(__dirname, 'out');
var problems = [];
var notes = [];

function note(s) { notes.push(s); process.stdout.write('  ' + s + '\n'); }
function check(cond, what) {
  if (cond) { process.stdout.write('  ok   ' + what + '\n'); } else {
    problems.push(what);
    process.stdout.write('  FAIL ' + what + '\n');
  }
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

function skip(why) {
  process.stdout.write('smoke: SKIP (' + why + ')\n');
  process.exit(0);
}

function main() {
  var pw = loadPlaywright();
  if (!pw) { skip('Playwright not found'); }
  var exe = findChromium();
  if (!fs.existsSync(OUT)) { fs.mkdirSync(OUT, { recursive: true }); }
  var srv = null;
  var browser = null;
  var page = null;
  var errors = [];
  var started = 0;

  function shot(name, full) {
    return page.waitForTimeout(250).then(function () { return page.screenshot({ path: path.join(OUT, name), fullPage: !!full }); }).then(function () { note('screenshot dev/out/' + name); });
  }

  function platformFonts(selector) {
    var cdp = null;
    return page.context().newCDPSession(page).then(function (s) {
      cdp = s;
      return cdp.send('DOM.enable');
    }).then(function () { return cdp.send('CSS.enable'); }).then(function () {
      return cdp.send('DOM.getDocument', { depth: -1 });
    }).then(function (doc) {
      return cdp.send('DOM.querySelector', { nodeId: doc.root.nodeId, selector: selector });
    }).then(function (r) {
      return cdp.send('CSS.getPlatformFontsForNode', { nodeId: r.nodeId });
    }).then(function (r) {
      return cdp.detach().then(function () { return r.fonts; });
    });
  }

  new Promise(function (resolve, reject) {
    serve.start(0, function (err, s) { if (err) { reject(err); } else { resolve(s); } });
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
    page.on('requestfailed', function (r) { errors.push('request failed: ' + r.url()); });
    page.on('response', function (r) { if (r.status() >= 400) { errors.push('HTTP ' + r.status() + ': ' + r.url()); } });
    started = Date.now();
    process.stdout.write('smoke: ' + srv.url + 'index.html?mock=1&debug=1 in ' + (exe || 'Playwright Chromium') + '\n');
    return page.goto(srv.url + 'index.html?mock=1&debug=1');
  }).then(function () {
    return page.waitForFunction(function () { return !!(window.VP_App && window.VP_App.ready()); }, null, { timeout: 15000, polling: 100 });
  }).then(function () {
    note('boot to ready: ' + (Date.now() - started) + ' ms (container Chromium, software rendering)');
    return page.evaluate(function () {
      return {
        title: document.title,
        lang: document.documentElement.lang,
        h1: document.querySelector('h1').textContent,
        line: document.getElementById('vp-engine-line').textContent,
        status: document.getElementById('vp-status').textContent,
        kind: window.VP_Bridge.kind(),
        missing: window.VP_I18n.missing()
      };
    });
  }).then(function (r) {
    check(r.kind === 'mock', 'bridge uses the mock engine (' + r.kind + ')');
    check(r.title === 'vetus poeta' && r.lang === 'en-US', 'title and <html lang> (' + r.title + ', ' + r.lang + ')');
    check(r.h1 === 'Welcome to vetus poeta', 'placeholder start screen mounted');
    check(r.line.indexOf('54,199') >= 0, 'engine status line: ' + r.line);
    check(r.status.indexOf('Offline') >= 0, 'status bar shows the offline indicator');
    check(r.missing.length === 0, 'no missing i18n keys at run time');
    return page.evaluate(function () {
      var before = window.VP_Debug.stats();
      for (var i = 0; i < 50; i++) { window.VP_Router.go('start'); }
      var after = window.VP_Debug.stats();
      return { before: before, after: after, failures: window.VP_Debug.failures() };
    });
  }).then(function (r) {
    var b = r.before;
    var a = r.after;
    check(a.listeners === b.listeners && a.timers === b.timers && a.domNodes === b.domNodes && JSON.stringify(a.caches) === JSON.stringify(b.caches),
      '50 mount/destroy cycles: listeners ' + b.listeners + '->' + a.listeners + ', timers ' + b.timers + '->' + a.timers + ', DOM nodes ' + b.domNodes + '->' + a.domNodes);
    check(r.failures.length === 0, 'no VP_Debug failures (router leak checks): ' + JSON.stringify(r.failures));
    check(a.domNodes <= 800, 'DOM nodes ' + a.domNodes + ' <= 800');
    return page.evaluate(function () {
      return document.fonts.ready.then(function () {
        var s = window.VP_App.fontSample();
        var faces = [];
        document.fonts.forEach(function (f) { faces.push(f.family + ' ' + f.style + ' ' + f.weight + ' ' + f.status); });
        return {
          la: document.fonts.check('16px "Gentium Plus"', s.la),
          grc: document.fonts.check('16px "Gentium Plus"', s.grc),
          mixed: document.fonts.check('16px "Gentium Plus"', s.la + ' ' + s.grc + ' ' + s.emoji),
          family: window.getComputedStyle(document.getElementById('vp-sample-grc')).fontFamily,
          faces: faces,
          heap: window.performance.memory ? window.performance.memory.usedJSHeapSize : 0
        };
      });
    });
  }).then(function (r) {
    check(r.la && r.grc && r.mixed, 'document.fonts.check("Gentium Plus") true for Latin, Greek and the mixed string with emoji');
    check(r.faces.some(function (f) { return /Gentium Plus normal 400 loaded/.test(f); }), 'Gentium Plus Regular face loaded (' + r.faces.join('; ') + ')');
    check(/^"Gentium Plus"/.test(r.family), 'computed font-family starts with Gentium Plus');
    if (r.heap) { check(r.heap <= 60 * 1048576, 'idle JS heap ' + (r.heap / 1048576).toFixed(1) + ' MB <= 60 MB'); }
    return platformFonts('#vp-sample-la');
  }).then(function (fonts) {
    check(fonts.length >= 1 && fonts.every(function (f) { return f.familyName === 'Gentium Plus'; }), 'Latin sample rendered only with Gentium Plus: ' + JSON.stringify(fonts));
    return platformFonts('#vp-sample-grc');
  }).then(function (fonts) {
    check(fonts.length >= 1 && fonts.every(function (f) { return f.familyName === 'Gentium Plus'; }), 'polytonic Greek sample rendered only with Gentium Plus: ' + JSON.stringify(fonts));
    return platformFonts('#vp-sample-emoji');
  }).then(function (fonts) {
    note('emoji span rendered with: ' + fonts.map(function (f) { return f.familyName + ' (' + f.glyphCount + ')'; }).join(', '));
    return page.evaluate(function () { window.VP_App.setTheme('light'); });
  }).then(function () {
    return shot('start-light-en.png', true);
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('auto'); });
  }).then(function () {
    return page.emulateMedia({ colorScheme: 'dark' });
  }).then(function () {
    return page.evaluate(function () { return window.getComputedStyle(document.documentElement).getPropertyValue('--bg').trim(); });
  }).then(function (bg) {
    check(bg.toUpperCase() === '#1B1815', 'theme "auto" follows the system dark setting (--bg ' + bg + ')');
    return page.evaluate(function () { window.VP_App.setTheme('dark'); });
  }).then(function () {
    return shot('start-dark-en.png', true);
  }).then(function () {
    return page.click('[data-lang="es-MX"]');
  }).then(function () {
    return page.evaluate(function () { return { lang: document.documentElement.lang, h1: document.querySelector('h1').textContent }; });
  }).then(function (r) {
    check(r.lang === 'es-MX' && r.h1 === 'Te damos la bienvenida a vetus poeta', 'language switch to es-MX by click');
    return shot('start-dark-es.png', true);
  }).then(function () {
    return page.keyboard.press('F1');
  }).then(function () {
    return page.evaluate(function () {
      var d = document.querySelector('[role="dialog"]');
      return { open: !!d, modal: d && d.getAttribute('aria-modal'), focusInside: !!d && d.contains(document.activeElement) };
    });
  }).then(function (r) {
    check(r.open && r.modal === 'true' && r.focusInside, 'F1 opens the shortcut dialog with focus inside');
    return shot('keys-dark-es.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_App.setTheme('light');
      window.VP_App.setLang('en-US');
      return window.VP_Dialog.count();
    });
  }).then(function (n) {
    check(n === 0, 'Escape closes the dialog');
    return page.focus('[data-action="tour"]');
  }).then(function () {
    return page.keyboard.press('Enter');
  }).then(function () {
    return page.keyboard.press('ArrowRight');
  }).then(function () {
    return page.evaluate(function () { return { active: window.VP_Tour.isActive(), index: window.VP_Tour.index(), title: document.querySelector('.vp-tour-title').textContent }; });
  }).then(function (r) {
    check(r.active && r.index === 1, 'tour started with Enter and moved with the arrow key (' + r.title + ')');
    return shot('tour-light-en.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_Toast.undoable('app.placeholder.toast.label', null, function () {});
      return { tour: window.VP_Tour.isActive(), status: !!document.querySelector('#vp-toasts[role="status"] .vp-toast') };
    });
  }).then(function (r) {
    check(!r.tour, 'Escape skips the tour');
    check(r.status, 'toast inside the role=status region');
    return shot('toast-light-en.png');
  }).then(function () {
    check(errors.length === 0, 'no console errors, CSP violations or failed requests' + (errors.length ? ': ' + errors.join(' | ') : ''));
  }).then(null, function (e) {
    problems.push('smoke crashed: ' + String(e && e.stack));
    process.stdout.write('  FAIL ' + String(e && e.stack) + '\n');
  }).then(function () {
    return browser ? browser.close() : null;
  }).then(function () {
    return new Promise(function (resolve) { if (srv) { srv.close(resolve); } else { resolve(); } });
  }).then(function () {
    if (problems.length) {
      process.stdout.write('smoke: FAIL (' + problems.length + ' problem(s))\n');
      process.exit(1);
    }
    process.stdout.write('smoke: PASS\n');
    process.exit(0);
  });
}

main();
