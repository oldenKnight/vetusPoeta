describe('VP_Export', function () {
  function boot() {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    env.app = D.el('div', { id: 'app', className: 'vp-app' });
    env.main = D.el('main', { id: 'vp-main' });
    env.app.appendChild(env.main);
    env.document.body.appendChild(env.app);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_App.boot({ root: env.main, status: D.el('div') });
    env.clock.tick(500);
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.cmds = function (name) { return env.sent.filter(function (s) { return s.cmd === name; }); };
    env.q = function (sel) { return env.document.querySelector(sel); };
    env.qa = function (sel) { return env.document.querySelectorAll(sel); };
    W.VP_Start.openSample();
    env.clock.tick(300);
    env.q('#vp-translate').click();
    env.clock.tick(3000);
    W.VP_Toast.clearAll();
    return env;
  }
  function cueWith(W, index, patch) {
    var c = JSON.parse(JSON.stringify(W.VP_Store.getCue(index)));
    for (var k in patch) { if (Object.prototype.hasOwnProperty.call(patch, k)) { c[k] = patch[k]; } }
    W.VP_Store.putCues([c]);
  }

  it('checks() counts cues, fast cues and Fix cues; fileName inserts the language code before the extension', function () {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_export.js']);
    var E = env.window.VP_Export;
    var r = E.checks([
      { index: 0, state: 'translated', confidence: 'ok', cps: 10, target: 'a' },
      { index: 1, state: 'translated', confidence: 'fix', cps: 25, target: 'b', flags: ['cps'] },
      { index: 2, state: 'reviewed', confidence: 'fix', cps: 5, target: 'c' },
      { index: 3, state: 'new', confidence: 'check', cps: 0, target: '' }
    ], 5, 17);
    eq(r.total, 5);
    eq(r.seen, 4);
    eq(r.complete, false);
    deepEq(r.fast, [1]);
    deepEq(r.fix, [1], 'a reviewed Fix cue was accepted by the teacher');
    eq(r.untranslated, 1);
    eq(E.fileName('Alicia.srt', 'en-la', 'srt'), 'Alicia.la.srt');
    eq(E.fileName('C:\\x\\Alicia.vpoeta', 'es-la', 'vtt'), 'Alicia.la.vtt');
    eq(E.fileName('Alicia.la.srt', 'en-la', 'ass'), 'Alicia.la.ass', 'no double code');
    eq(E.fileName('Odes.txt', 'en-grc', 'txt'), 'Odes.grc.txt');
  });

  it('the dialog: original format on, name with the code, options from the settings, checks with Show them / Review first, preview of 3 cues, Export writes', function () {
    var env = boot();
    var W = env.window;
    cueWith(W, 4, { confidence: 'fix' });
    cueWith(W, 5, { cps: 30, flags: ['cps'] });
    env.q('#vp-export-btn').click();
    env.clock.tick(100);
    ok(W.VP_Export.isOpen());
    eq(env.q('.vp-dialog-title').textContent, 'Export');
    eq(env.q('input[data-exp-format="srt"]').checked, true);
    eq(env.q('input[data-exp-format="vtt"]').checked, false);
    eq(env.q('#vp-exp-name').value, 'sample.en.la.srt');
    eq(env.q('#vp-exp-emoji').checked, false, 'D9: emoji off in the file');
    eq(env.q('#vp-exp-macrons').checked, false, 'D13: macrons off in the file');
    eq(env.q('#vp-exp-rebreak').checked, true, 'D15');
    eq(env.q('.vp-exp-greek').getAttribute('aria-disabled'), 'true', 'the Greek choice is shown but off for a Latin target');
    eq(env.q('input[data-exp-greek="monotonic"]').disabled, true);
    eq(env.q('#vp-exp-greek-off').textContent, 'Only for translations into Ancient Greek.');
    var checks = env.qa('.vp-exp-check').map(function (li) { return li.querySelector('.vp-exp-check-text').textContent; });
    eq(checks[0], '12 cues, numbering and timing unchanged');
    eq(checks[1], '1 cue faster than 17 characters per second');
    eq(checks[2], '1 cue marked Fix');
    var exportBtn = env.q('[data-exp-action="run"]');
    ok(exportBtn.disabled, 'red cues need the explicit tick');
    var prev = env.cmds('export.preview');
    eq(prev.length, 1);
    deepEq(prev[0].params.indices, [0, 1, 2]);
    eq(prev[0].params.macrons, false);
    eq(env.qa('.vp-exp-preview .vp-preview-line').length, 3);
    ok(env.qa('.vp-exp-preview .vp-preview-line')[0].textContent.indexOf('ā') < 0, 'preview without macrons');
    env.q('#vp-exp-macrons').checked = true;
    env.fire(env.q('#vp-exp-macrons'), 'change');
    env.clock.tick(50);
    eq(env.cmds('export.preview').length, 2);
    eq(env.cmds('export.preview')[1].params.macrons, true);
    eq(W.VP_Store.get('settings')['export'].macrons, false, 'options change this export only');
    env.q('#vp-exp-anyway').checked = true;
    env.fire(env.q('#vp-exp-anyway'), 'change');
    ok(!exportBtn.disabled);
    env.qa('[data-exp-action="showFast"]')[0].click();
    env.clock.tick(50);
    eq(W.VP_Export.isOpen(), false);
    eq(W.VP_CueList.filter(), 'fast');
    env.q('#vp-export-btn').click();
    env.clock.tick(100);
    env.qa('[data-exp-action="reviewFix"]')[0].click();
    env.clock.tick(50);
    eq(W.VP_CueList.filter(), 'fix');
    eq(W.VP_CueList.selected(), 4);
    W.VP_CueList.setFilter('all');
    env.q('#vp-export-btn').click();
    env.clock.tick(100);
    W.VP_Export.setAnyway(true);
    env.q('input[data-exp-format="vtt"]').checked = true;
    env.fire(env.q('input[data-exp-format="vtt"]'), 'change');
    eq(env.q('#vp-exp-name').value, 'sample.en.la.vtt');
    env.q('[data-exp-action="run"]').click();
    env.clock.tick(100);
    var w = env.cmds('export.write');
    eq(w.length, 1);
    eq(w[0].params.path, 'mock://data/samples/sample.en.la.vtt');
    eq(w[0].params.format, 'vtt');
    eq(w[0].params.overwrite, undefined);
    eq(w[0].params.rebreak, true);
    eq(W.VP_Export.isOpen(), false);
    eq(env.q('.vp-toast-text').textContent, 'Exported “sample.en.la.vtt”.');
    env.q('.vp-toast-action').click();
    env.clock.tick(50);
    eq(env.cmds('shell.revealFile')[0].params.path, 'mock://data/samples/sample.en.la.vtt');
    deepEq(W.VP_I18n.missing(), []);
    eq(env.errors().length, 0);
  });

  it('an existing file: io "file exists" opens the confirm; Replace retries with overwrite:true, cancel writes nothing; Choose... uses dialog.saveFile', function () {
    var env = boot();
    var W = env.window;
    W.VP_Export.open();
    env.clock.tick(100);
    env.q('[data-exp-action="choose"]').click();
    env.clock.tick(50);
    eq(env.cmds('dialog.saveFile')[0].params.suggestedName, 'sample.en.la.srt');
    eq(env.q('#vp-exp-name').value, 'sample.en.la.srt');
    env.q('#vp-exp-name').value = 'exists.la.srt';
    env.fire(env.q('#vp-exp-name'), 'change');
    env.q('[data-exp-action="run"]').click();
    env.clock.tick(100);
    eq(env.cmds('export.write').length, 1);
    eq(W.VP_Dialog.count(), 2, 'confirm over the export dialog');
    eq(W.VP_Dialog.top().el.querySelector('.vp-dialog-title').textContent, 'Replace the file?');
    ok(W.VP_Dialog.top().el.querySelector('.vp-dialog-body').textContent.indexOf('exists.la.srt') > 0);
    W.VP_Dialog.top().el.querySelector('[data-dialog-action="0"]').click();
    env.clock.tick(100);
    eq(env.cmds('export.write').length, 1, 'cancel: no second write');
    ok(W.VP_Export.isOpen(), 'the export dialog stays open');
    env.q('[data-exp-action="run"]').click();
    env.clock.tick(100);
    W.VP_Dialog.top().el.querySelector('[data-dialog-action="1"]').click();
    env.clock.tick(100);
    var w = env.cmds('export.write');
    eq(w.length, 3);
    eq(w[2].params.overwrite, true);
    eq(w[2].params.path, 'mock://data/samples/exists.la.srt');
    eq(W.VP_Export.isOpen(), false);
    W.VP_Toast.clearAll();
    W.VP_Workspace.close();
    env.clock.tick(300);
    deepEq(W.VP_Debug.failures(), []);
  });

  it('B8 Greek target: the polytonic / monotonic choice is enabled, monotonic goes to export.preview and export.write, the dialog carries the Greek accent', function () {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    var app = D.el('div', { id: 'app', className: 'vp-app' });
    var main = D.el('main', { id: 'vp-main' });
    app.appendChild(main);
    env.document.body.appendChild(app);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_App.boot({ root: main, status: D.el('div') });
    env.clock.tick(500);
    var sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    var q = function (s) { return env.document.querySelector(s); };
    q('#vp-start-pair').value = 'en-grc';
    env.fire(q('#vp-start-pair'), 'change');
    W.VP_Start.openSample();
    env.clock.tick(300);
    q('#vp-translate').click();
    env.clock.tick(3000);
    W.VP_Toast.clearAll();
    W.VP_Export.open();
    env.clock.tick(100);
    eq(q('.vp-exp-greek').getAttribute('aria-disabled'), null);
    eq(q('#vp-exp-greek-off'), null);
    var mono = q('input[data-exp-greek="monotonic"]');
    eq(mono.disabled, false);
    eq(q('input[data-exp-greek="polytonic"]').checked, true, 'polytonic by default (D13)');
    ok(q('.vp-dialog-export').classList.contains('pair-grc'));
    eq(q('#vp-exp-name').value, 'sample.en.grc.srt');
    mono.checked = true;
    env.fire(mono, 'change', { bubbles: true });
    env.clock.tick(100);
    var prev = sent.filter(function (s) { return s.cmd === 'export.preview'; });
    eq(prev[prev.length - 1].params.greek, 'monotonic');
    eq(q('.vp-exp-preview .vp-preview-line').getAttribute('lang'), 'grc');
    ok(q('.vp-exp-preview .vp-preview-line').textContent.indexOf('κόρη') >= 0);
    W.VP_Export.run();
    env.clock.tick(200);
    var w = sent.filter(function (s) { return s.cmd === 'export.write'; });
    eq(w.length, 1);
    eq(w[0].params.greek, 'monotonic');
    eq(env.errors().length, 0, JSON.stringify(env.errors()));
  });
});
