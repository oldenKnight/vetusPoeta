/* smoke.js - DEV ONLY browser smoke test (PREDESIGN 6.2/6.3). Needs Playwright and a
 * Chromium; prints SKIP and exits 0 when either is missing.
 *   node gui/ui/dev/smoke.js     (NODE_PATH or /opt/node-tools/node_modules for Playwright,
 *                                 PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers for Chromium)
 * Checks: no console errors, CSP violations or failed requests; boot with the mock engine;
 * 50 mounts/destroys of the start screen return VP_Debug.stats() to baseline; Gentium Plus
 * renders the Latin (macrons, breves) and polytonic Greek sample (document.fonts.check plus
 * Chromium's own platform-font report, on the B3 placeholder screen mounted for it); auto
 * dark follows the system; DOM and heap budgets; shortcut dialog and tour by keyboard.
 * Screens (B6): the sample project opens in the workspace, a mock translation runs to the
 * end and leaves "Needs review (n)" with the first cue to review selected; the editor
 * underlines an unknown word; the drawer below 1180 px; a 50,000-cue project is paged in,
 * scrolled to the end and back (rendered cue rows <= 40, DOM nodes <= 800); selecting a cue
 * takes < 100 ms (to the next frame); closing a project returns listeners and timers to the
 * Start screen baseline. Screenshots go to gui/ui/dev/out/ (gitignored): start and
 * workspace in light/dark and en-US/es-MX.
 * Panels (B7): every right-panel tab opens (Word with the inspector on a word, "why" and
 * the paradigm table open; Engines; Names; Corrections; Words), the Export, Settings and
 * About dialogs open and close, Orbergise mode runs on a Latin file, the six-step tour runs
 * to its end on the start screen and its last step opens the sample; screenshots of each
 * tab and dialog in light/dark and en/es; listeners and timers return to the baseline.
 * B8: a reading pair (la-en: Latin source words as chips with the hover card, the reading in
 * the Word tab, Ctrl+I interlinear lines, plain English target) and a Greek target (en-grc:
 * lang="grc", Aegean accent, the ";" question mark, Gentium Plus in the preview strip and the
 * cue list, the engine warning chip of a requested model that is missing); screenshots
 * reading-la-en-light-en.png, greek-light-en.png.
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

function window_tour_version() { return '1'; }

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

  var baseline = null;

  // Selecting a cue: VP_CueList.select() renders the row and the panes synchronously; the
  // time runs until the next animation frame, so it includes style and layout.
  function measureSelect(n, what) {
    return page.evaluate(function (count) {
      var total = window.VP_CueList.total();
      var times = [];
      var i = 0;
      return new Promise(function (resolve) {
        function one() {
          if (i >= count) {
            resolve(times);
            return;
          }
          var index = Math.floor((i * 7919) % total);
          i++;
          var t0 = performance.now();
          window.VP_CueList.select(index);
          var row = document.querySelector('.vp-cue-row-selected');
          var height = row ? row.offsetHeight : 0; // reading it forces style and layout
          var sync = performance.now() - t0;
          requestAnimationFrame(function () {
            times.push([performance.now() - t0, sync, height]);
            setTimeout(one, 30);
          });
        }
        one();
      });
    }, n).then(function (pairs) {
      var frame = pairs.map(function (p) { return p[0]; }).sort(function (a, b) { return a - b; });
      var sync = pairs.map(function (p) { return p[1]; }).sort(function (a, b) { return a - b; });
      var max = frame[frame.length - 1];
      check(max < 100, 'selecting a cue (' + what + '): to the next frame median ' + frame[Math.floor(frame.length / 2)].toFixed(1) + ' ms, max ' + max.toFixed(1) +
        ' ms (< 100 ms); render plus layout median ' + sync[Math.floor(sync.length / 2)].toFixed(1) + ' ms, max ' + sync[sync.length - 1].toFixed(1) + ' ms');
    });
  }

  function compareBaseline(when) {
    return page.waitForTimeout(100).then(function () {
      return page.evaluate(function () { return window.VP_Debug.stats(); });
    }).then(function (s) {
      check(s.listeners === baseline.listeners && s.timers === baseline.timers, when + ': listeners ' + baseline.listeners + '->' + s.listeners + ', timers ' + baseline.timers + '->' + s.timers + ' (baseline of the Start screen)');
      check(s.cueRows === 0 && s.domNodes <= baseline.domNodes + 10, when + ': ' + s.domNodes + ' DOM nodes (Start baseline ' + baseline.domNodes + ' plus a recent-project row when the project had a file), no cue rows');
    });
  }

  function shot(name, full) {
    return page.waitForTimeout(250).then(function () { return page.screenshot({ path: path.join(OUT, name), fullPage: !!full }); }).then(function () { note('screenshot dev/out/' + name); });
  }

  // B8 helpers: open the sample of a pair from the start screen and translate it; close.
  function openPairSample(pair) {
    return page.evaluate(function (p) {
      var sel = document.getElementById('vp-start-pair');
      sel.value = p;
      sel.dispatchEvent(new Event('change', { bubbles: true }));
    }, pair).then(function () {
      return page.click('[data-start-action="sample"]');
    }).then(function () {
      return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_Store.cueCount() === 12; }, null, { timeout: 10000, polling: 50 });
    }).then(function () {
      return page.click('#vp-translate');
    }).then(function () {
      return page.waitForFunction(function () { return !window.VP_Store.get('job') && (window.VP_Store.get('cueCounts') || {}).translated === 12; }, null, { timeout: 15000, polling: 50 });
    });
  }

  function closeProject() {
    return page.click('.vp-ws-home').then(function () {
      return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 5000, polling: 50 });
    });
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
        line: document.getElementById('vp-start-status').textContent,
        status: document.getElementById('vp-status').textContent,
        kind: window.VP_Bridge.kind(),
        missing: window.VP_I18n.missing()
      };
    });
  }).then(function (r) {
    check(r.kind === 'mock', 'bridge uses the mock engine (' + r.kind + ')');
    check(r.title === 'vetus poeta' && r.lang === 'en-US', 'title and <html lang> (' + r.title + ', ' + r.lang + ')');
    check(r.h1 === 'What would you like to do?', 'start screen mounted (' + r.h1 + ')');
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
      '50 mount/destroy cycles of Start: listeners ' + b.listeners + '->' + a.listeners + ', timers ' + b.timers + '->' + a.timers + ', DOM nodes ' + b.domNodes + '->' + a.domNodes);
    check(r.failures.length === 0, 'no VP_Debug failures (router leak checks): ' + JSON.stringify(r.failures));
    check(a.domNodes <= 800, 'DOM nodes ' + a.domNodes + ' <= 800');
    return page.evaluate(function () {
      // The font sample lives on the B3 placeholder screen, mounted here only for this check.
      if (!window.VP_Router.has('fonts')) { window.VP_Router.register('fonts', window.VP_App.placeholder); }
      window.VP_Router.go('fonts');
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
    return page.evaluate(function () {
      window.VP_Toast.clearAll();
      window.VP_Router.go('start');
      window.VP_App.setTheme('light');
    });
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
    check(r.lang === 'es-MX' && r.h1 === '¿Qué quieres hacer?', 'language switch to es-MX by click');
    return shot('start-dark-es.png', true);
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('light'); });
  }).then(function () {
    return shot('start-light-es.png', true);
  }).then(function () {
    return page.keyboard.press('F1');
  }).then(function () {
    return page.evaluate(function () {
      var d = document.querySelector('[role="dialog"]');
      return { open: !!d, modal: d && d.getAttribute('aria-modal'), focusInside: !!d && d.contains(document.activeElement) };
    });
  }).then(function (r) {
    check(r.open && r.modal === 'true' && r.focusInside, 'F1 opens the shortcut dialog with focus inside');
    return shot('keys-light-es.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_App.setLang('en-US');
      return window.VP_Dialog.count();
    });
  }).then(function (n) {
    check(n === 0, 'Escape closes the dialog');
    return page.evaluate(function () { return window.VP_Debug.stats(); });
  }).then(function (s0) {
    baseline = s0;
    note('Start screen baseline: ' + s0.listeners + ' listeners, ' + s0.timers + ' timers, ' + s0.domNodes + ' DOM nodes');
    return page.click('[data-start-action="sample"]');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_Store.cueCount() === 12; }, null, { timeout: 10000, polling: 50 });
  }).then(function () {
    return page.evaluate(function () {
      return { name: document.querySelector('.vp-ws-name').textContent, rows: window.VP_Debug.stats().cueRows, src: document.getElementById('vp-src-text').textContent };
    });
  }).then(function (r) {
    check(r.name === 'sample.en.srt' && r.rows === 12, 'sample project opens in the workspace (' + r.name + ', ' + r.rows + ' rows)');
    check(r.src === 'The girl sees the rose.', 'first cue selected and shown: ' + r.src);
    return page.click('#vp-translate');
  }).then(function () {
    return page.waitForFunction(function () { return !window.VP_Store.get('job') && window.VP_CueList.filter() === 'review'; }, null, { timeout: 15000, polling: 50 });
  }).then(function () {
    return page.evaluate(function () {
      var L = window.VP_CueList;
      var sel = document.getElementById('vp-cl-filter');
      return { selected: L.selected(), first: L.firstReview(), label: sel.options[sel.selectedIndex].textContent, review: L.counts().review, counts: document.querySelector('.vp-ws-counts').textContent };
    });
  }).then(function (r) {
    check(r.first >= 0 && r.selected === r.first, 'after translating, the first cue to review is selected (' + r.selected + ')');
    check(r.label === 'Needs review (' + r.review + ')', 'filter shows "' + r.label + '"');
    check(r.counts.indexOf('12 / 12 translated') === 0, 'status bar counts: ' + r.counts);
    return page.evaluate(function () { window.VP_Toast.clearAll(); });
  }).then(function () {
    return shot('workspace-light-en.png');
  }).then(function () {
    return page.click('.vp-word');
  }).then(function () {
    return page.waitForTimeout(150);
  }).then(function () {
    return shot('workspace-word-light-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('dark'); });
  }).then(function () {
    return shot('workspace-dark-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setLang('es-MX'); });
  }).then(function () {
    return shot('workspace-dark-es.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('light'); });
  }).then(function () {
    return shot('workspace-light-es.png');
  }).then(function () {
    // B7: the right-panel tabs with the inspector on a word
    return page.evaluate(function () {
      window.VP_Panes.dismiss();
      window.VP_Panes.openWord(0);
      return new Promise(function (resolve) { setTimeout(resolve, 200); }).then(function () {
        window.VP_Inspector.toggleWhy(true);
        window.VP_Inspector.toggleForms(true);
        return new Promise(function (resolve) { setTimeout(resolve, 200); });
      }).then(function () {
        var st = window.VP_Inspector.state();
        return { tab: window.VP_Workspace.tab(), word: st.word, loaded: st.loaded, blocks: document.querySelectorAll('.vp-why-block').length, table: !!document.querySelector('.vp-paradigm'), used: document.querySelectorAll('.vp-par-used').length };
      });
    });
  }).then(function (r) {
    check(r.tab === 'word' && !!r.word && r.loaded, 'Word tab: inspector on "' + r.word + '"');
    check(r.blocks === 4 && r.table && r.used === 1, 'four "why this word" blocks, paradigm table with the used cell highlighted');
    return shot('panel-word-light-es.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('dark'); window.VP_App.setLang('en-US'); });
  }).then(function () {
    return shot('panel-word-dark-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('light'); });
  }).then(function () {
    var tabs = ['engines', 'names', 'corrections', 'words'];
    var chain = Promise.resolve();
    tabs.forEach(function (t) {
      chain = chain.then(function () { return page.click('#vp-ptab-' + t); }).then(function () { return page.waitForTimeout(300); }).then(function () {
        return page.evaluate(function (id) {
          var mod = { engines: 'VP_Engines', names: 'VP_Names', corrections: 'VP_Corrections', words: 'VP_Words' }[id];
          return { tab: window.VP_Workspace.tab(), mounted: window[mod].isMounted(), text: document.getElementById('vp-panel-body').textContent.length, nodes: window.VP_Debug.stats().domNodes };
        }, t);
      }).then(function (r) {
        check(r.tab === t && r.mounted && r.text > 20 && r.nodes <= 800, t + ' tab mounted (' + r.text + ' characters, ' + r.nodes + ' DOM nodes)');
        return shot('panel-' + t + '-light-en.png');
      });
    });
    return chain;
  }).then(function () {
    return page.click('#vp-ptab-engines');
  }).then(function () {
    return page.waitForTimeout(200);
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setLang('es-MX'); window.VP_App.setTheme('dark'); });
  }).then(function () {
    return shot('panel-engines-dark-es.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setLang('en-US'); window.VP_App.setTheme('light'); window.VP_Workspace.setTab('word'); });
  }).then(function () {
    // Export, Settings, About dialogs
    return page.keyboard.press('Control+Shift+E');
  }).then(function () {
    return page.waitForTimeout(400);
  }).then(function () {
    return page.evaluate(function () { return { open: window.VP_Export.isOpen(), title: document.querySelector('.vp-dialog-title').textContent, preview: document.querySelectorAll('.vp-exp-preview .vp-preview-line').length, focus: document.activeElement.id }; });
  }).then(function (r) {
    check(r.open && r.title === 'Export' && r.preview >= 3, 'Ctrl+Shift+E opens Export with a 3-cue preview (focus on ' + r.focus + ')');
    return shot('dialog-export-light-en.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    return page.keyboard.press('Control+,');
  }).then(function () {
    return page.waitForTimeout(300);
  }).then(function () {
    return page.evaluate(function () { return { open: window.VP_Settings.isOpen(), sections: document.querySelectorAll('.vp-set-section').length, exportClosed: !window.VP_Export.isOpen() }; });
  }).then(function (r) {
    check(r.open && r.sections === 6 && r.exportClosed, 'Ctrl+, opens Settings with six sections; Escape closed Export');
    return shot('dialog-settings-light-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('dark'); window.VP_App.setLang('es-MX'); });
  }).then(function () {
    return shot('dialog-settings-dark-es.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_App.setTheme('light'); window.VP_App.setLang('en-US'); window.VP_Settings.close(); window.VP_About.open('data'); });
  }).then(function () {
    return page.waitForTimeout(300);
  }).then(function () {
    return page.evaluate(function () { return { open: window.VP_About.isOpen(), notices: document.querySelectorAll('.vp-about-notice').length, text: document.querySelector('.vp-about-notice').textContent }; });
  }).then(function (r) {
    check(r.open && r.notices === 2 && r.text.indexOf('Wiktionary') > 0, 'About shows the lexicon notices verbatim');
    return shot('dialog-about-light-en.png');
  }).then(function () {
    return page.evaluate(function () { window.VP_About.close(); return window.VP_Dialog.count(); });
  }).then(function (n) {
    check(n === 0, 'every dialog closed');
    return page.evaluate(function () { window.VP_App.setLang('es-MX'); });
  }).then(function () {
    return page.keyboard.press('e');
  }).then(function () {
    return page.keyboard.type(' xyzzy');
  }).then(function () {
    return page.waitForTimeout(600);
  }).then(function () {
    return page.evaluate(function () { return { mode: window.VP_Panes.mode(), marks: document.querySelectorAll('.vp-editor-mirror .vp-unknown').length }; });
  }).then(function (r) {
    check(r.mode === 'edit' && r.marks === 1, 'E opens the editor; the unknown word is underlined (' + r.marks + ')');
    return shot('workspace-editor-light-es.png');
  }).then(function () {
    return page.keyboard.press('Escape');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_App.setLang('en-US');
      window.VP_CueList.setFilter('all');
      window.VP_CueList.focusList();
    });
  }).then(function () {
    return measureSelect(12, 'sample');
  }).then(function () {
    return page.setViewportSize({ width: 1100, height: 760 });
  }).then(function () {
    return page.click('.vp-ws-drawer-btn');
  }).then(function () {
    return page.evaluate(function () { var r = document.querySelector('.vp-ws-right'); return { open: window.VP_Workspace.drawerOpen(), shown: r.getBoundingClientRect().width > 0 }; });
  }).then(function (r) {
    check(r.open && r.shown, 'below 1180 px the right panel is a drawer opened by its button');
    return shot('workspace-drawer-1100.png');
  }).then(function () {
    return page.setViewportSize({ width: 1280, height: 800 });
  }).then(function () {
    return page.click('.vp-ws-home');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 5000, polling: 50 });
  }).then(function () {
    return compareBaseline('after closing the sample project');
  }).then(function () {
    // Orbergise mode on a Latin file (la-la): three panes, meaning chip, changed words
    return page.evaluate(function () {
      document.getElementById('vp-start-orberg').checked = true;
      document.getElementById('vp-start-orberg').dispatchEvent(new Event('change', { bubbles: true }));
      window.VP_Start.openSample();
    });
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_Workspace.mode() === 'orberg' && window.VP_Store.cueCount() === 12; }, null, { timeout: 10000, polling: 50 });
  }).then(function () {
    return page.click('#vp-orb-run');
  }).then(function () {
    return page.waitForFunction(function () { return !window.VP_Store.get('job'); }, null, { timeout: 15000, polling: 50 });
  }).then(function () {
    return page.evaluate(function () {
      window.VP_Toast.clearAll();
      window.VP_CueList.select(1);
      return new Promise(function (resolve) { setTimeout(resolve, 300); }).then(function () {
        return { panes: document.querySelectorAll('.vp-orb-body .vp-pane').length, changed: document.querySelectorAll('#vp-orb-version .vp-word-changed').length, chip: document.querySelector('.vp-orb-chip').textContent, nodes: window.VP_Debug.stats().domNodes };
      });
    });
  }).then(function (r) {
    check(r.panes === 3 && r.changed === 1 && /67/.test(r.chip), 'Orbergise mode: three panes, one changed word, meaning chip "' + r.chip + '", ' + r.nodes + ' DOM nodes');
    return shot('orberg-light-en.png');
  }).then(function () {
    return page.click('#vp-orb-version .vp-word-changed');
  }).then(function () {
    return page.waitForTimeout(300);
  }).then(function () {
    return page.evaluate(function () { return { tab: window.VP_Workspace.tab(), text: document.getElementById('vp-panel-body').textContent }; });
  }).then(function (r) {
    check(r.tab === 'word' && r.text.indexOf('habitat') >= 0 && r.text.indexOf('vīvit') >= 0, 'clicking a changed word shows "was -> now" in the Word tab');
    return shot('orberg-word-light-en.png');
  }).then(function () {
    return page.click('.vp-ws-home');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 5000, polling: 50 });
  }).then(function () {
    return page.evaluate(function () { document.getElementById('vp-start-orberg').checked = false; });
  }).then(function () {
    return compareBaseline('after closing the Orbergise project');
  }).then(function () {
    // B8: a reading pair, la-en
    return openPairSample('la-en');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_CueList.setFilter('all');
      window.VP_CueList.select(0);
      return new Promise(function (resolve) { setTimeout(resolve, 400); }).then(function () {
        return { chips: document.querySelectorAll('#vp-src-text .vp-src-word').length, tgtChips: document.querySelectorAll('#vp-target-view .vp-word').length, tgt: document.getElementById('vp-target-view').textContent };
      });
    });
  }).then(function (r) {
    check(r.chips === 3 && r.tgtChips === 0 && r.tgt === 'The girl sees the rose.', 'la-en: 3 Latin word chips in the source pane, plain English target "' + r.tgt + '"');
    return page.hover('#vp-src-text .vp-src-word >> nth=1');
  }).then(function () {
    return page.waitForTimeout(300);
  }).then(function () {
    return page.evaluate(function () { var c = document.getElementById('vp-src-card'); return { shown: !c.hidden, text: c.textContent }; });
  }).then(function (r) {
    check(r.shown && r.text.indexOf('rosa') === 0 && r.text.indexOf('rose') > 0, 'hovering a Latin word shows its card: ' + r.text);
    return page.click('#vp-src-text .vp-src-word >> nth=1');
  }).then(function () {
    return page.keyboard.press('Control+i');
  }).then(function () {
    return page.waitForTimeout(400);
  }).then(function () {
    return page.evaluate(function () {
      window.VP_Inspector.toggleWhy(true);
      window.VP_Toast.clearAll();
      return { tab: window.VP_Workspace.tab(), side: window.VP_Inspector.state().side, blocks: document.querySelectorAll('#vp-panel-body .vp-why-block').length, il: document.querySelectorAll('#vp-src-text .vp-il-gloss').length, nodes: document.getElementsByTagName('*').length };
    });
  }).then(function (r) {
    check(r.tab === 'word' && r.side === 'analysis' && r.blocks === 3, 'clicking a Latin word shows "Why this reading?" in the Word tab (' + r.blocks + ' blocks)');
    check(r.il === 3, 'Ctrl+I shows the interlinear lines under the 3 source words; ' + r.nodes + ' DOM nodes');
    return shot('reading-la-en-light-en.png');
  }).then(function () {
    return page.keyboard.press('Control+i');
  }).then(function () {
    return closeProject();
  }).then(function () {
    return compareBaseline('after closing the la-en project');
  }).then(function () {
    // B8: a Greek target, en-grc, with the local model asked for but missing
    return page.evaluate(function () { return window.VP_App.saveSettings({ engines: { model: true, online: false } }); });
  }).then(function () {
    return openPairSample('en-grc');
  }).then(function () {
    return page.evaluate(function () {
      window.VP_CueList.setFilter('all');
      window.VP_CueList.select(11);
      return new Promise(function (resolve) { setTimeout(resolve, 400); }).then(function () {
        var line = document.querySelector('.vp-preview-line');
        var row = document.querySelector('.vp-cue-row [lang="grc"]');
        var accent = getComputedStyle(document.querySelector('.vp-ws')).getPropertyValue('--accent').trim();
        return {
          tgt: document.getElementById('vp-target-view').textContent, lang: document.getElementById('vp-target-view').getAttribute('lang'), pairGrc: document.querySelector('.vp-ws').classList.contains('pair-grc'), accent: accent,
          lineLang: line && line.getAttribute('lang'), lineFont: line ? getComputedStyle(line).fontFamily : '', rowFont: row ? getComputedStyle(row).fontFamily : '',
          check: document.fonts.check('16px "Gentium Plus"', (line ? line.textContent : '') + (row ? row.textContent : '')),
          chip: Array.prototype.map.call(document.querySelectorAll('.vp-ws-warning'), function (c) { return c.textContent; }).join(' | ')
        };
      });
    });
  }).then(function (r) {
    check(r.tgt === 'πῶς ἔχεις, ὦ φίλε;' && r.lang === 'grc', 'Greek target with lang="grc" and its ";" question mark: ' + r.tgt);
    check(r.pairGrc && r.accent.toUpperCase() !== '#B3452A', 'Greek pair: the workspace root carries pair-grc, accent ' + r.accent);
    check(r.lineLang === 'grc' && /Gentium Plus/.test(r.lineFont) && /Gentium Plus/.test(r.rowFont) && r.check, 'preview strip and cue list render Greek with Gentium Plus (document.fonts.check ' + r.check + ')');
    check(r.chip === 'Model unavailable: not installed', 'status chip for the missing local model: ' + r.chip);
    return platformFonts('.vp-preview-line');
  }).then(function (fonts) {
    check(fonts.length >= 1 && fonts.every(function (f) { return f.familyName === 'Gentium Plus'; }), 'Greek preview line rendered only with Gentium Plus: ' + JSON.stringify(fonts));
    return page.evaluate(function () { window.VP_Toast.clearAll(); });
  }).then(function () {
    return shot('greek-light-en.png');
  }).then(function () {
    return page.click('.vp-ws-warning');
  }).then(function () {
    return page.waitForTimeout(300);
  }).then(function () {
    return page.evaluate(function () { return { tab: window.VP_Workspace.tab(), text: document.getElementById('vp-panel-body').textContent }; });
  }).then(function (r) {
    check(r.tab === 'engines' && r.text.indexOf('Last translation: Model unavailable: not installed') >= 0, 'the warning chip opens the Engines tab with the warning');
    return shot('greek-engines-light-en.png');
  }).then(function () {
    return page.evaluate(function () { return window.VP_App.saveSettings({ engines: { model: false, online: false } }); });
  }).then(function () {
    return closeProject();
  }).then(function () {
    return page.evaluate(function () {
      var sel = document.getElementById('vp-start-pair');
      sel.value = 'en-la';
      sel.dispatchEvent(new Event('change', { bubbles: true }));
    });
  }).then(function () {
    return compareBaseline('after closing the Greek project');
  }).then(function () {
    // The six-step tour on the start screen, to the end, then the sample from its last step
    return page.evaluate(function () { window.VP_App.startTour(); return { active: window.VP_Tour.isActive(), spot: !document.querySelector('.vp-tour-spot').hidden, title: document.querySelector('.vp-tour-title').textContent }; });
  }).then(function (r) {
    check(r.active && r.spot && r.title === 'Open a subtitle file', 'the real tour starts on the drop zone');
    return shot('tour-step1-light-en.png');
  }).then(function () {
    var chain = Promise.resolve();
    for (var i = 0; i < 5; i++) { chain = chain.then(function () { return page.keyboard.press('ArrowRight'); }).then(function () { return page.waitForTimeout(100); }); }
    return chain;
  }).then(function () {
    return page.evaluate(function () { return { index: window.VP_Tour.index(), action: !document.querySelector('.vp-tour-action').hidden, title: document.querySelector('.vp-tour-title').textContent }; });
  }).then(function (r) {
    check(r.index === 5 && r.action, 'last step "' + r.title + '" offers the sample project');
    return shot('tour-step6-light-en.png');
  }).then(function () {
    return page.click('.vp-tour-action');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_Store.cueCount() === 12; }, null, { timeout: 10000, polling: 50 });
  }).then(function () {
    return page.evaluate(function () { return { tour: window.VP_Tour.isActive(), seen: window.VP_Store.get('settings').tourSeenVersion }; });
  }).then(function (r) {
    check(!r.tour && r.seen === window_tour_version(), 'the tour ends, opens the sample and is marked seen (' + r.seen + ')');
    return page.click('.vp-ws-home');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 5000, polling: 50 });
  }).then(function () {
    return compareBaseline('after the tour and the sample');
  }).then(function () {
    started = Date.now();
    return page.evaluate(function () { window.VP_Start.openPath('C:\\Users\\Teacher\\demo-50000-cues.vpoeta'); });
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'workspace' && window.VP_CueList.isMounted() && window.VP_CueList.allLoaded(); }, null, { timeout: 60000, polling: 100 });
  }).then(function () {
    note('50,000-cue project open and every cue paged in: ' + (Date.now() - started) + ' ms');
    return page.evaluate(function () {
      var v = document.querySelector('.vp-cl-viewport');
      var worst = { rows: 0, nodes: 0 };
      var offsets = [0];
      for (var i = 1; i <= 40; i++) { offsets.push(Math.floor(v.scrollHeight * i / 40)); }
      offsets.push(v.scrollHeight);
      for (var k = offsets.length - 1; k >= 0; k--) { offsets.push(offsets[k]); }
      var i2 = 0;
      return new Promise(function (resolve) {
        function next() {
          if (i2 >= offsets.length) {
            resolve(worst);
            return;
          }
          v.scrollTop = offsets[i2++];
          requestAnimationFrame(function () {
            requestAnimationFrame(function () {
              var s = window.VP_Debug.stats();
              worst.rows = Math.max(worst.rows, s.cueRows);
              worst.nodes = Math.max(worst.nodes, s.domNodes);
              if (i2 === 42) {
                var rows = document.querySelectorAll('.vp-cue-row');
                worst.lastIndex = Number(rows[rows.length - 1].getAttribute('data-index'));
              }
              next();
            });
          });
        }
        next();
      }).then(function (w) {
        w.backTop = v.scrollTop;
        w.firstIndex = Number(document.querySelector('.vp-cue-row').getAttribute('data-index'));
        w.heap = window.performance.memory ? window.performance.memory.usedJSHeapSize : 0;
        w.total = document.querySelector('.vp-cl-viewport').getAttribute('aria-rowcount');
        return w;
      });
    });
  }).then(function (w) {
    check(w.total === '50000', 'listbox aria-rowcount 50000');
    check(w.rows <= 40, 'scrolling 50,000 cues to the end and back: at most ' + w.rows + ' rendered cue rows (<= 40)');
    check(w.nodes <= 800, 'DOM nodes at most ' + w.nodes + ' (<= 800)');
    check(w.lastIndex === 49999, 'the last cue is rendered at the end (' + w.lastIndex + ')');
    check(w.backTop === 0 && w.firstIndex === 0, 'back at the top');
    if (w.heap) { note('JS heap with 50,000 cues loaded: ' + (w.heap / 1048576).toFixed(1) + ' MB'); }
    return shot('workspace-50k-light-en.png');
  }).then(function () {
    return measureSelect(30, '50,000 cues');
  }).then(function () {
    return page.evaluate(function () {
      var t0 = performance.now();
      window.VP_CueList.setFilter('review');
      var t1 = performance.now();
      window.VP_CueList.setQuery('insula');
      var t2 = performance.now();
      var n = window.VP_CueList.viewIndices().length;
      window.VP_CueList.setQuery('');
      window.VP_CueList.setFilter('all');
      return { filterMs: t1 - t0, searchMs: t2 - t1, n: n };
    });
  }).then(function (r) {
    note('50,000 cues: filter "Needs review" ' + r.filterMs.toFixed(1) + ' ms, search "insula" ' + r.searchMs.toFixed(1) + ' ms (' + r.n + ' matches)');
    check(r.n > 0, 'macron-insensitive search finds "īnsulā" in 50,000 cues');
    return page.click('.vp-ws-home');
  }).then(function () {
    return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 10000, polling: 50 });
  }).then(function () {
    return compareBaseline('after closing the 50,000-cue project');
  }).then(function () {
    return page.evaluate(function () { return { failures: window.VP_Debug.failures(), missing: window.VP_I18n.missing(), cues: window.VP_Store.cueCount() }; });
  }).then(function (r) {
    check(r.failures.length === 0, 'no VP_Debug failures (router leak checks) over the whole run: ' + JSON.stringify(r.failures));
    check(r.missing.length === 0, 'no missing i18n keys: ' + r.missing.join(', '));
    check(r.cues === 0, 'cue records dropped on close');
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
