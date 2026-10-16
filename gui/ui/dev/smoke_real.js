/* smoke_real.js - DEV ONLY browser smoke test of the UI against the REAL engine (vpengine serve), through
 * engine_bridge_shim.js. Needs Playwright + Chromium (like smoke.js) and the engine with its data; prints SKIP and
 * exits 0 when any of them is missing.
 *   node gui/ui/dev/smoke_real.js [--engine build/engine/cli/vpengine] [--lexicons data/work]
 * Steps: boot (bridge kind "webview", no router.leak: the bridge is initialised before the first screen), start
 * screen with the engine line and the pair picker built from engine.hello (pairs enabled, pairsUnavailable disabled
 * with their reason in the note), en-la: "Try the sample" opens the hello.samples entry, Translate runs to the end,
 * the Word tab with the inspector on the first word ("why this word" and the paradigm table), the Words tab, the
 * Export dialog with its preview. la-en (B8): the Latin sample, translated with the local model and the online check
 * asked for (the engine answers translate.warning: toast + status chips that open the Engines tab), Latin source
 * words as chips with the hover card, "Why this reading?" in the Word tab, Ctrl+I interlinear lines, the plain
 * English target, the export preview = the readable sentences. grc-en / en-grc / la-la run when hello.pairs lists
 * them (Greek: lang="grc", pair-grc, Gentium Plus in the preview strip and cue list, the monotonic export preview;
 * la-la: the Orbergise panes; B10: no identical was/now pair in the change list of any cue, the meaning chip on
 * every cue (without and with sample.en.srt), "Choose file…" with Detect shows the engine's detected language and
 * the aligned line, Forget unloads it; a text project of our own shows a change and a "words kept" count),
 * else they are skipped with the engine's reason. Screenshots: gui/ui/dev/out/real-*.png (gitignored).
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

  // ---- B8 flows ----
  var real = { pairs: [], unavailable: [], baseline: null };

  function wait(ms) { return page.waitForTimeout(ms); }

  function closeProject(what) {
    return page.evaluate(function () { window.VP_Toast.clearAll(); }).then(function () {
      return page.click('.vp-ws-home');
    }).then(function () {
      return page.waitForFunction(function () { return window.VP_Router.current() === 'start'; }, null, { timeout: 10000, polling: 100 });
    }).then(function () {
      return wait(300);
    }).then(function () {
      return page.evaluate(function () { return window.VP_Debug.stats(); });
    }).then(function (st) {
      if (!real.baseline) { real.baseline = st; }
      check(st.listeners === real.baseline.listeners && st.timers === real.baseline.timers, 'after closing the ' + what + ' project: listeners ' + st.listeners + ', timers ' + st.timers + ' (Start baseline ' + real.baseline.listeners + '/' + real.baseline.timers + ')');
    });
  }

  function openPairSample(pair, orberg) {
    return page.evaluate(function (a) {
      var sel = document.getElementById('vp-start-pair');
      var box = document.getElementById('vp-start-orberg');
      if (!a.orberg) {
        sel.value = a.pair;
        sel.dispatchEvent(new Event('change', { bubbles: true }));
      }
      if (box.checked !== !!a.orberg) {
        box.checked = !!a.orberg;
        box.dispatchEvent(new Event('change', { bubbles: true }));
      }
      return window.VP_Start.samplePath(a.orberg ? 'la-la' : a.pair);
    }, { pair: pair, orberg: !!orberg }).then(function (sample) {
      note(pair + ': sample ' + sample);
      return page.click('[data-start-action="sample"]');
    }).then(function () {
      return page.waitForFunction(function (p) {
        var pr = window.VP_Store.get('project');
        return window.VP_Router.current() === 'workspace' && pr && pr.pair === p && window.VP_Store.cueCount() >= 1 && window.VP_Store.cueCount() === window.VP_Store.cueTotal();
      }, orberg ? 'la-la' : pair, { timeout: 20000, polling: 100 });
    });
  }

  function translateAll() {
    return page.click('#vp-translate').then(function () {
      return page.waitForFunction(function () {
        var c = window.VP_Store.get('cueCounts') || {};
        return !window.VP_Store.get('job') && c.translated === window.VP_Store.cueTotal();
      }, null, { timeout: 60000, polling: 100 });
    });
  }

  function optionalPair(pair, fn) {
    if (real.pairs.indexOf(pair) >= 0) { return fn(); }
    return page.evaluate(function (p) { return window.VP_Start.pairReason(window.VP_Start.pairInfo(p)); }, pair).then(function (why) {
      note('SKIP ' + pair + ': not in engine.hello.pairs (' + why + ')');
    });
  }

  // la-en / grc-en: source chips, card, reading, interlinear, plain target, warnings, export preview.
  function readingPair(pair) {
    var tag = pair.replace('-', '-');
    return page.evaluate(function () { return window.VP_App.saveSettings({ engines: { model: true, online: true } }); }).then(function () {
      return openPairSample(pair);
    }).then(function () {
      return page.click('#vp-translate');
    }).then(function () {
      return page.waitForFunction(function () { return document.querySelectorAll('.vp-toast-text').length > 0; }, null, { timeout: 10000, polling: 50 });
    }).then(function () {
      return page.evaluate(function () { return Array.prototype.map.call(document.querySelectorAll('.vp-toast-text'), function (t) { return t.textContent; }); });
    }).then(function (toasts) {
      check(/unavailable/.test(toasts[0] || ''), pair + ': the first toast is the engine warning: ' + toasts.join(' | '));
      return page.waitForFunction(function () {
        var c = window.VP_Store.get('cueCounts') || {};
        return !window.VP_Store.get('job') && c.translated === window.VP_Store.cueTotal();
      }, null, { timeout: 60000, polling: 100 });
    }).then(function () {
      return page.evaluate(function () {
        window.VP_Toast.clearAll();
        window.VP_CueList.setFilter('all');
        window.VP_CueList.select(0);
        return new Promise(function (r) { setTimeout(r, 800); }).then(function () {
          return {
            chips: document.querySelectorAll('#vp-src-text .vp-src-word').length, toks: window.VP_Panes.sourceTokens().length,
            tgtChips: document.querySelectorAll('#vp-target-view .vp-word').length, tgt: document.getElementById('vp-target-view').textContent,
            target: window.VP_Store.getCue(0).target, src: document.getElementById('vp-src-text').textContent, srcLang: document.getElementById('vp-src-text').getAttribute('lang'),
            warn: Array.prototype.map.call(document.querySelectorAll('.vp-ws-warning'), function (c) { return c.textContent; })
          };
        });
      });
    }).then(function (r) {
      check(r.chips > 0 && r.chips === r.toks, pair + ': the ' + r.srcLang + ' source "' + r.src + '" as ' + r.chips + ' word chips (cue.get source-tokens)');
      check(r.tgtChips === 0 && r.tgt === r.target, pair + ': plain readable target "' + r.tgt + '"');
      check(r.warn.length === 2, pair + ': status chips ' + r.warn.join(' | '));
      return page.hover('#vp-src-text .vp-src-word >> nth=1');
    }).then(function () {
      return wait(600);
    }).then(function () {
      return page.evaluate(function () { var c = document.getElementById('vp-src-card'); return { shown: !c.hidden, text: c.textContent }; });
    }).then(function (r) {
      check(r.shown && r.text.length > 3, pair + ': hover card "' + r.text + '"');
      return page.click('#vp-src-text .vp-src-word >> nth=1');
    }).then(function () {
      return wait(800);
    }).then(function () {
      return page.evaluate(function () {
        window.VP_Inspector.toggleWhy(true);
        window.VP_Inspector.toggleForms(true);
        return new Promise(function (r) { setTimeout(r, 800); }).then(function () {
          var body = document.getElementById('vp-panel-body');
          return { tab: window.VP_Workspace.tab(), side: window.VP_Inspector.state().side, blocks: body.querySelectorAll('.vp-why-block').length, table: !!body.querySelector('.vp-paradigm'), text: body.textContent.replace(/\s+/g, ' ').slice(0, 500) };
        });
      });
    }).then(function (r) {
      check(r.tab === 'word' && r.side === 'analysis' && r.blocks === 3, pair + ': "Why this reading?" in the Word tab (' + r.blocks + ' blocks, paradigm ' + r.table + ')');
      note(pair + ' Word tab: ' + r.text);
      return page.keyboard.press('Control+i');
    }).then(function () {
      return wait(800);
    }).then(function () {
      return page.evaluate(function () { window.VP_Toast.clearAll(); return { il: document.querySelectorAll('#vp-src-text .vp-il-gloss').length, chips: document.querySelectorAll('#vp-src-text .vp-src-word').length, glosses: Array.prototype.map.call(document.querySelectorAll('#vp-src-text .vp-il-gloss'), function (g) { return g.textContent; }).join(' | ') }; });
    }).then(function (r) {
      check(r.il === r.chips && r.il > 0, pair + ': Ctrl+I interlinear lines (' + r.glosses + ')');
      return shot('real-' + tag + '-light-en.png');
    }).then(function () {
      return page.keyboard.press('Control+i');
    }).then(function () {
      return page.keyboard.press('Control+Shift+E');
    }).then(function () {
      return wait(1200);
    }).then(function () {
      return page.evaluate(function () {
        var lines = Array.prototype.map.call(document.querySelectorAll('.vp-exp-preview .vp-preview-line'), function (l) { return l.textContent; });
        var want = [0, 1, 2].map(function (i) { return (window.VP_Store.getCue(i).lines || []).join('\n'); }).join('\n');
        return { lines: lines, want: want };
      });
    }).then(function (r) {
      check(r.lines.join('\n') === r.want, pair + ': export writes the readable text (preview: ' + r.lines.join(' | ') + ')');
      return page.keyboard.press('Escape');
    }).then(function () {
      return page.click('.vp-ws-warning >> nth=0');
    }).then(function () {
      return wait(600);
    }).then(function () {
      return page.evaluate(function () { return { tab: window.VP_Workspace.tab(), text: document.getElementById('vp-panel-body').textContent.replace(/\s+/g, ' ') }; });
    }).then(function (r) {
      var m = /Last translation:[^.]*\./.exec(r.text);
      check(r.tab === 'engines' && !!m, pair + ': the chip opens the Engines tab: ' + (m ? m[0] : r.text.slice(0, 200)));
      check(/English or Spanish only/.test(r.text) || /without the local model/.test(r.text), pair + ': model copy for rerankEnabled:false / not built');
      return shot('real-' + tag + '-engines-light-en.png');
    }).then(function () {
      return page.evaluate(function () { return window.VP_App.saveSettings({ engines: { model: false, online: false } }); });
    }).then(function () {
      return closeProject(pair);
    });
  }

  function greekTarget() {
    return openPairSample('en-grc').then(function () {
      return translateAll();
    }).then(function () {
      return page.evaluate(function () {
        window.VP_Toast.clearAll();
        window.VP_CueList.setFilter('all');
        window.VP_CueList.select(0);
        return new Promise(function (r) { setTimeout(r, 800); }).then(function () {
          var line = document.querySelector('.vp-preview-line');
          var row = document.querySelector('.vp-cue-row [lang="grc"]');
          return {
            tgt: document.getElementById('vp-target-view').textContent, lang: document.getElementById('vp-target-view').getAttribute('lang'), grc: document.querySelector('.vp-ws').classList.contains('pair-grc'),
            fonts: [line ? getComputedStyle(line).fontFamily : '', row ? getComputedStyle(row).fontFamily : ''], check: document.fonts.check('16px "Gentium Plus"', (line ? line.textContent : '') + (row ? row.textContent : ''))
          };
        });
      });
    }).then(function (r) {
      check(r.lang === 'grc' && r.grc, 'en-grc: Greek target "' + r.tgt + '" with lang="grc" and the Greek accent');
      check(/Gentium Plus/.test(r.fonts[0]) && /Gentium Plus/.test(r.fonts[1]) && r.check, 'en-grc: preview strip and cue list in Gentium Plus (document.fonts.check ' + r.check + ')');
      return shot('real-greek-light-en.png');
    }).then(function () {
      return page.keyboard.press('Control+Shift+E');
    }).then(function () {
      return wait(1000);
    }).then(function () {
      return page.evaluate(function () { var m = document.querySelector('input[data-exp-greek="monotonic"]'); return !!m && !m.disabled; });
    }).then(function (on) {
      check(on, 'en-grc: the monotonic export option is enabled for a Greek target');
      return page.click('input[data-exp-greek="monotonic"]');
    }).then(function () {
      return wait(1200);
    }).then(function () {
      // C8b: export.preview {greek:"monotonic"} through vp::grc::toMonotonic (no breathing, circumflex, grave, iota subscript)
      return page.evaluate(function () {
        return Array.prototype.map.call(document.querySelectorAll('.vp-exp-preview .vp-preview-line'), function (l) { return l.textContent; });
      });
    }).then(function (lines) {
      var text = lines.join(' ');
      var poly = /[\u1F00-\u1FFF]/.test(text) || /[\u0300\u0313\u0314\u0342\u0345]/.test(text.normalize('NFD'));
      check(lines.length > 0 && /[\u0370-\u03FF]/.test(text) && !poly, 'en-grc: monotonic export preview (' + lines.slice(0, 3).join(' | ') + ')');
      return shot('real-greek-export-monotonic-light-en.png');
    }).then(function () {
      return page.keyboard.press('Escape');
    }).then(function () {
      return closeProject('en-grc');
    });
  }

  function orbergise() {
    return openPairSample('la-la', true).then(function () {
      return page.evaluate(function () { return window.VP_Workspace.mode(); });
    }).then(function (mode) {
      check(mode === 'orberg', 'la-la opens in Orbergise mode');
      return page.click('#vp-orb-run');
    }).then(function () {
      return page.waitForFunction(function () { return !window.VP_Store.get('job') && (window.VP_Store.get('cueCounts') || {}).translated === window.VP_Store.cueTotal(); }, null, { timeout: 60000, polling: 100 });
    }).then(function () {
      return wait(800);
    }).then(function () {
      return page.evaluate(function () { return { panes: document.querySelectorAll('.vp-orb-body .vp-pane').length, ver: document.getElementById('vp-orb-version').textContent, chip: document.querySelector('.vp-orb-chip').textContent }; });
    }).then(function (r) {
      check(r.panes === 3 && r.ver.length > 0, 'la-la: Orberg version "' + r.ver + '", ' + r.chip);
      return orbergCues('without an original');
    }).then(function () {
      return shot('real-orberg-light-en.png');
    }).then(function () {
      return orbergOriginal();
    }).then(function () {
      return closeProject('la-la');
    }).then(function () {
      return orbergText();
    });
  }

  // B10: every cue of the la-la sample: the change list never shows an identical pair (the engine's
  // "structure kept" reasons, was == now, are only counted), the meaning chip is there.
  function orbergCues(what) {
    var seen = { cues: 0, changes: 0, kept: 0, identical: [], noChip: [] };
    function one(i) {
      return page.evaluate(function (k) { window.VP_CueList.select(k); }, i).then(function () {
        return page.waitForFunction(function (k) { var st = window.VP_Orberg.stats(); return st && st.index === k && document.querySelector('.vp-orb-chip') && !document.querySelector('.vp-orb-chip').hidden; }, i, { timeout: 5000, polling: 100 }).then(null, function () { return null; });
      }).then(function () {
        return wait(250);
      }).then(function () {
        return page.evaluate(function () {
          var out = { pairs: [], titles: [], kept: 0, chip: null };
          var items = document.querySelectorAll('.vp-orb-changes .vp-orb-change');
          for (var j = 0; j < items.length; j++) { out.pairs.push([items[j].querySelector('.vp-orb-was').textContent, items[j].querySelector('.vp-orb-now').textContent]); }
          var under = document.querySelectorAll('#vp-orb-version .vp-word-changed');
          for (j = 0; j < under.length; j++) { out.titles.push(under[j].getAttribute('title') || ''); }
          var chip = document.querySelector('.vp-orb-chip');
          out.chip = chip && !chip.hidden ? chip.textContent : null;
          out.kept = window.VP_Orberg.kept().length;
          out.underlinedSame = window.VP_Orberg.changes().filter(function (c) { return c.was === c.now; }).length;
          return out;
        });
      }).then(function (r) {
        seen.cues++;
        seen.changes += r.pairs.length;
        seen.kept += r.kept;
        r.pairs.forEach(function (pr) { if (pr[0] === pr[1]) { seen.identical.push((i + 1) + ': ' + pr[0] + ' -> ' + pr[1]); } });
        r.titles.forEach(function (t) { var m = /^Was (.*), now (.*)$/.exec(t); if (m && m[1] === m[2]) { seen.identical.push((i + 1) + ': underlined ' + t); } });
        if (r.underlinedSame) { seen.identical.push((i + 1) + ': ' + r.underlinedSame + ' was == now in changes()'); }
        if (!r.chip || !/\d+\s?%/.test(r.chip)) { seen.noChip.push((i + 1) + ': ' + r.chip); }
      });
    }
    return page.evaluate(function () { window.VP_CueList.setFilter('all'); return window.VP_Store.cueTotal(); }).then(function (n) {
      var chain = Promise.resolve();
      for (var i = 0; i < Math.min(n, 12); i++) { chain = chain.then(one.bind(null, i)); }
      return chain;
    }).then(function () {
      check(seen.identical.length === 0, 'la-la ' + what + ': the change list shows no identical pairs over ' + seen.cues + ' cues (' + seen.changes + ' changes listed, ' + seen.kept + ' words kept only counted)' + (seen.identical.length ? ': ' + seen.identical.join(' / ') : ''));
      check(seen.noChip.length === 0, 'la-la ' + what + ': the meaning chip with a percentage on every cue' + (seen.noChip.length ? ' (missing: ' + seen.noChip.join(' / ') + ')' : ''));
      return page.evaluate(function () { window.VP_CueList.select(0); });
    }).then(function () {
      return wait(400);
    }).then(function () {
      return seen;
    });
  }

  // B10 on sentences of our own where the engine reports both kinds: an ablative absolute rewritten as a
  // postquam clause (a change) and "putat" kept with its structure (was == now: counted, never listed).
  function orbergText() {
    var text = 'Urbe captā, mīlitēs praedam dīvīsērunt.\n\nMārcus putat puellam rosam amāre.';
    return page.evaluate(function (t) {
      var box = document.getElementById('vp-start-orberg');
      box.checked = true;
      box.dispatchEvent(new Event('change', { bubbles: true }));
      window.VP_Start.startText(t);
    }, text).then(function () {
      return page.waitForFunction(function () {
        var pr = window.VP_Store.get('project');
        return window.VP_Router.current() === 'workspace' && pr && pr.pair === 'la-la' && window.VP_Store.cueCount() === 2;
      }, null, { timeout: 20000, polling: 100 });
    }).then(function () {
      return page.click('#vp-orb-run');
    }).then(function () {
      return wait(300);
    }).then(function () {
      return page.waitForFunction(function () { return !window.VP_Store.get('job') && (window.VP_Store.get('cueCounts') || {}).translated === 2; }, null, { timeout: 60000, polling: 100 });
    }).then(function () {
      return wait(800);
    }).then(function () {
      return orbergCues('on our own text');
    }).then(function (seen) {
      check(seen.changes >= 1 && seen.kept >= 1, 'la-la on our own text: ' + seen.changes + ' change(s) listed, ' + seen.kept + ' word(s) kept counted, not listed');
      return page.evaluate(function () {
        window.VP_CueList.select(1);
      });
    }).then(function () {
      return wait(800);
    }).then(function () {
      return page.evaluate(function () { var k = document.querySelector('.vp-orb-kept'); return { kept: k && !k.hidden ? k.textContent : '', list: document.querySelectorAll('.vp-orb-changes .vp-orb-change').length }; });
    }).then(function (r) {
      check(/kept/.test(r.kept) && r.list === 0, 'la-la: "putat" (was == now) only in the counter "' + r.kept + '", ' + r.list + ' change(s) listed');
      return shot('real-orberg-kept-light-en.png');
    }).then(function () {
      return closeProject('la-la text');
    });
  }

  // B10: "Choose file…" with Detect (the shim answers dialog.openFile with sample.en.srt): no originalLang
  // is sent, the pane shows the engine's detected language and the aligned original line; Forget unloads it.
  function orbergOriginal() {
    function jobDone() {
      return wait(300).then(function () {
        return page.waitForFunction(function () { return !window.VP_Store.get('job'); }, null, { timeout: 60000, polling: 100 });
      }).then(function () { return wait(800); });
    }
    return page.evaluate(function () { window.VP_Toast.clearAll(); return document.getElementById('vp-orb-lang').value; }).then(function (v) {
      check(v === '', 'la-la: the original\'s language selector defaults to Detect');
      return page.click('[data-orb-action="choose"]');
    }).then(function () {
      return page.waitForFunction(function () { return !!window.VP_Orberg.original(); }, null, { timeout: 20000, polling: 100 });
    }).then(jobDone).then(function () {
      return page.evaluate(function () {
        var f = document.querySelector('.vp-orb-file');
        return { original: window.VP_Orberg.original(), file: f && !f.hidden ? f.textContent : null, text: document.querySelector('.vp-orb-orig').textContent, forget: !document.querySelector('[data-orb-action="forget"]').hidden };
      });
    }).then(function (r) {
      check(r.original && r.original.lang === 'en' && r.original.detected && /English \(detected\)/.test(r.file || ''), 'la-la: original ' + (r.file || 'none') + ' (detected by the engine)');
      check(r.text.length > 0 && r.text.indexOf('not loaded') < 0 && r.forget, 'la-la: the aligned original line "' + r.text + '" and a Forget button');
      return shot('real-orberg-original-light-en.png');
    }).then(function () {
      return orbergCues('with sample.en.srt');
    }).then(function () {
      return page.evaluate(function () { window.VP_Toast.clearAll(); });
    }).then(function () {
      return page.click('[data-orb-action="forget"]');
    }).then(function () {
      return page.waitForFunction(function () { return !window.VP_Orberg.original(); }, null, { timeout: 20000, polling: 100 });
    }).then(jobDone).then(function () {
      return page.evaluate(function () { return { orberg: window.VP_Store.get('project').orberg, choose: !document.querySelector('[data-orb-action="choose"]').hidden }; });
    }).then(function (r) {
      check(!r.orberg.originalPath && r.choose, 'la-la: Forget unloads the original (project.orberg.originalPath null, "Choose file…" back)');
    });
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
    return page.evaluate(function () {
      var hello = window.VP_Store.get('engine').hello;
      var opts = Array.prototype.map.call(document.querySelectorAll('#vp-start-pair option'), function (o) { return { pair: o.value, off: o.disabled, title: o.getAttribute('title') || '' }; });
      var note = Array.prototype.map.call(document.querySelectorAll('#vp-start-pair-note li'), function (li) { return li.textContent; });
      return { pairs: hello.pairs || [], unavailable: (hello.pairsUnavailable || []).map(function (u) { return u.pair; }), opts: opts, note: note, orberg: document.getElementById('vp-start-orberg').disabled, samples: (hello.samples || []).map(function (x) { return x.lang; }), stats: window.VP_Debug.stats() };
    });
  }).then(function (r) {
    real.pairs = r.pairs;
    real.baseline = r.stats;
    note('Start screen baseline: ' + r.stats.listeners + ' listeners (the bridge one included), ' + r.stats.timers + ' timers');
    real.unavailable = r.unavailable;
    note('engine.hello pairs: ' + r.pairs.join(', ') + '; unavailable: ' + r.unavailable.join(', ') + '; samples: ' + r.samples.join(', '));
    var agree = r.opts.every(function (o) { return o.off === (r.pairs.indexOf(o.pair) < 0); });
    check(agree, 'pair picker follows hello.pairs: ' + r.opts.map(function (o) { return o.pair + (o.off ? ' (off: ' + o.title + ')' : ''); }).join(', '));
    check(r.orberg === (r.pairs.indexOf('la-la') < 0), 'Orbergise option ' + (r.orberg ? 'disabled (la-la not offered)' : 'enabled'));
    check(r.unavailable.length === 0 || r.note.length > 0, 'the note under the picker gives the reasons: ' + r.note.join(' / '));
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
    return closeProject('en-la');
  }).then(function () {
    return readingPair('la-en');
  }).then(function () {
    return optionalPair('grc-en', function () { return readingPair('grc-en'); });
  }).then(function () {
    return optionalPair('en-grc', greekTarget);
  }).then(function () {
    return optionalPair('la-la', orbergise);
  }).then(function () {
    return page.evaluate(function () { return window.VP_Debug.failures(); });
  }).then(function (f) {
    check(f.length === 0, 'no VP_Debug failures (router.leak with the WebView2 transport included): ' + JSON.stringify(f));
    check(errors.length === 0, 'no console errors or failed requests' + (errors.length ? ': ' + errors.slice(0, 5).join(' / ') : ''));
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
