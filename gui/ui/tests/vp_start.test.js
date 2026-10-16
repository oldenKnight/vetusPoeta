describe('VP_Start', function () {
  function boot(before) {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    env.app = D.el('div', { id: 'app', className: 'vp-app' });
    env.main = D.el('main', { id: 'vp-main' });
    env.status = D.el('div', { id: 'vp-status' });
    env.app.appendChild(env.main);
    env.app.appendChild(env.status);
    env.document.body.appendChild(env.app);
    W.VP_MockEngine.options.latencyMs = 1;
    if (before) { before(env); }
    W.VP_App.boot({ root: env.main, status: env.status });
    env.clock.tick(500);
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.cmds = function (name) { return env.sent.filter(function (s) { return s.cmd === name; }); };
    env.q = function (sel) { return env.document.querySelector(sel); };
    return env;
  }
  function seedRecent(n) {
    return function (env) {
      var paths = [];
      var info = {};
      var now = new Date().getTime();
      for (var i = 0; i < n; i++) {
        var p = 'C:\\Users\\Teacher\\Projects\\Fabula ' + (i + 1) + '.vpoeta';
        paths.push(p);
        info[p] = { name: 'Fabula ' + (i + 1) + '.vpoeta', pair: i % 2 ? 'es-la' : 'en-la', kind: 'subs', cues: 1019, translated: i === 1 ? 1019 : 842, needReview: i === 1 ? 0 : 12, at: now - (i * i * 37 + 2) * 60000 };
      }
      env.window.localStorage.setItem('vp.mock.settings', JSON.stringify({ recentProjects: paths, recentInfo: info }));
    };
  }

  it('lays out PREDESIGN 1.1: two cards, pair picker from engine.hello (unavailable pairs disabled with the reason), Orbergise option, sample, status line', function () {
    var env = boot();
    var W = env.window;
    eq(W.VP_Router.current(), 'start');
    eq(env.q('h1').textContent, 'What would you like to do?');
    eq(env.document.querySelectorAll('h1').length, 1);
    eq(env.q('#vp-start-file-title').textContent, 'Subtitle file');
    eq(env.q('[data-start-action="choose"]').textContent, 'Choose file…');
    eq(env.q('#vp-start-text-title').textContent, 'Type or paste text');
    var options = env.document.querySelectorAll('#vp-start-pair option');
    deepEq(options.map(function (o) { return o.value; }), ['en-la', 'es-la', 'la-en', 'la-es', 'en-grc', 'es-grc', 'grc-en', 'grc-es']);
    deepEq(options.map(function (o) { return o.disabled; }), [false, false, false, false, false, true, false, true], 'the mock offers en-grc and grc-en');
    eq(options[5].textContent, 'Spanish → Ancient Greek (not available)');
    eq(options[5].getAttribute('title'), 'This language pair is not available yet.');
    eq(options[4].textContent, 'English → Ancient Greek');
    eq(options[1].textContent, 'Spanish → Latin');
    var note = env.document.querySelectorAll('#vp-start-pair-note li');
    eq(note.length, 1);
    eq(note[0].textContent, 'Spanish → Ancient Greek, Ancient Greek → Spanish: This language pair is not available yet.');
    eq(env.q('#vp-start-pair').getAttribute('aria-describedby'), 'vp-start-pair-note');
    eq(env.q('#vp-start-orberg').disabled, false);
    var line = env.q('#vp-start-status').textContent;
    ok(line.indexOf('Latin dictionary mock-1: 54,199 entries') >= 0, line);
    ok(line.indexOf('Local model not installed (optional)') >= 0, line);
    ok(line.indexOf('Online check off') >= 0, line);
    ok(env.q('.vp-card-error').hidden, 'no red card when the dictionary is there');
    eq(env.q('.vp-recent-empty').hidden, false);
    env.document.querySelectorAll('button').forEach(function (b) {
      ok(b.textContent.replace(/\s+/g, '') || b.getAttribute('aria-label'), 'button without a name: ' + b.className);
    });
    env.document.querySelectorAll('input, textarea, select').forEach(function (f) {
      ok(f.id && env.q('label[for="' + f.id + '"]'), 'field without a label: ' + f.id);
    });
    env.q('[data-lang="es-MX"]').click();
    eq(env.q('h1').textContent, '¿Qué quieres hacer?');
    eq(env.document.querySelectorAll('#vp-start-pair option')[5].textContent, 'Español → griego antiguo (no disponible)');
    eq(env.document.querySelectorAll('#vp-start-pair-note li')[0].textContent, 'Español → griego antiguo, Griego antiguo → español: Este par de idiomas todavía no está disponible.');
    ok(env.q('#vp-start-status').textContent.indexOf('Diccionario de latín') >= 0);
    deepEq(W.VP_I18n.missing(), []);
  });

  it('recent projects: at most 20 real buttons with pair, progress, review count and relative time; a click opens the project', function () {
    var env = boot(seedRecent(25));
    var W = env.window;
    var rows = env.document.querySelectorAll('.vp-recent-row');
    eq(rows.length, 20, 'capped at 20');
    eq(rows[0].localName, 'button');
    eq(rows[0].getAttribute('type'), 'button');
    var t0 = rows[0].textContent;
    ok(t0.indexOf('Fabula 1.vpoeta') === 0, t0);
    ok(t0.indexOf('English → Latin') > 0, t0);
    ok(t0.indexOf('842 / 1,019 cues') > 0, t0);
    ok(t0.indexOf('12 need review') > 0, t0);
    ok(t0.indexOf('2 min ago') > 0, t0);
    ok(rows[1].textContent.indexOf('done') > 0 && rows[1].textContent.indexOf('Spanish → Latin') > 0, rows[1].textContent);
    ok(rows[3].textContent.indexOf('h ago') > 0, rows[3].textContent);
    ok(rows[19].textContent.indexOf('/') > 0, rows[19].textContent);
    eq(W.VP_Start.recentRows().length, 20);
    rows[2].click();
    env.clock.tick(300);
    deepEq(env.cmds('project.open')[0].params, { path: 'C:\\Users\\Teacher\\Projects\\Fabula 3.vpoeta' });
    eq(W.VP_Router.current(), 'workspace');
    eq(W.VP_Store.get('project').name, 'Fabula 3.vpoeta');
    eq(W.VP_Store.get('settings').recentProjects[0], 'C:\\Users\\Teacher\\Projects\\Fabula 3.vpoeta', 'engine moved it to the top');
  });

  it('recovery banner when project.open reports newer autosaved work: Recover (default) or keep the saved version', function () {
    var env = boot();
    var W = env.window;
    W.VP_Start.openPath('C:\\x\\after-crash.vpoeta');
    env.clock.tick(100);
    eq(W.VP_Router.current(), 'start', 'stays on Start while asking');
    var banner = env.q('.vp-banner');
    ok(!banner.hidden);
    ok(banner.textContent.indexOf('We found work from your last session.') === 0, banner.textContent);
    ok(banner.textContent.indexOf('after-crash.vpoeta has changes autosaved 8 min ago') > 0, banner.textContent);
    eq(env.document.activeElement.getAttribute('data-start-action'), 'recover', 'Recover has the focus (default)');
    ok(env.document.activeElement.classList.contains('vp-btn-primary'));
    env.document.activeElement.click();
    env.clock.tick(300);
    deepEq(env.cmds('project.recover')[0].params, { path: 'C:\\x\\after-crash.vpoeta' });
    eq(W.VP_Router.current(), 'workspace');
    W.VP_Workspace.close();
    env.clock.tick(300);
    W.VP_Start.openPath('C:\\x\\after-crash.vpoeta');
    env.clock.tick(100);
    env.q('[data-start-action="keep"]').click();
    env.clock.tick(300);
    eq(env.cmds('project.recover').length, 1, 'keeping the saved version does not recover');
    eq(W.VP_Router.current(), 'workspace');
  });

  it('a missing Latin dictionary shows one red card with the exact fix', function () {
    var env = boot(function (e) { e.window.VP_MockEngine.options.noLexicon = true; });
    var card = env.q('.vp-card-error');
    ok(!card.hidden);
    eq(card.getAttribute('role'), 'alert');
    ok(card.textContent.indexOf('The dictionary is missing') === 0, card.textContent);
    ok(card.textContent.indexOf('Reinstall vetus poeta to restore its dictionary files.') > 0);
    ok(card.textContent.indexOf('Expected file: mock://data/lexicons/latin.vpl') > 0, card.textContent);
    ok(env.q('#vp-start-status').textContent.indexOf('No dictionary installed') >= 0);
    var env2 = boot(function (e) { e.window.VP_MockEngine.options.failNext = { cmd: 'engine.hello', code: 'lexicon_corrupt' }; });
    ok(!env2.q('.vp-card-error').hidden);
    ok(env2.q('.vp-card-error').textContent.indexOf('The dictionary file is damaged') === 0);
  });

  it('Choose file, the sample, typed text and dropped files start a project; Orbergise picks la-la', function () {
    var env = boot();
    var W = env.window;
    env.q('[data-start-action="choose"]').click();
    env.clock.tick(300);
    eq(env.cmds('dialog.openFile').length, 1);
    deepEq(env.cmds('project.new')[0].params, { kind: 'subs', pair: 'en-la', sourcePath: 'C:\\Users\\Student\\Videos\\lesson-3.srt' });
    eq(W.VP_Router.current(), 'workspace');
    W.VP_Workspace.close();
    env.clock.tick(300);
    env.q('#vp-start-pair').value = 'es-la';
    env.q('[data-start-action="sample"]').click();
    env.clock.tick(300);
    deepEq(env.cmds('project.new')[1].params, { kind: 'subs', pair: 'es-la', sourcePath: 'mock://data/samples/sample.es.srt' });
    eq(W.VP_Store.get('project').cues, 12);
    eq(W.VP_Store.getCue(0).source, 'La niña ve la rosa.');
    W.VP_Workspace.close();
    env.clock.tick(300);
    env.q('[data-start-action="text"]').click();
    env.clock.tick(50);
    ok(!env.q('#vp-start-text-error').hidden, 'empty text explains what to do');
    eq(env.q('#vp-start-text').getAttribute('aria-invalid'), 'true');
    eq(env.cmds('project.new').length, 2);
    env.q('#vp-start-text').value = 'The girl sees the rose.\n\nThe wolf runs.';
    env.q('[data-start-action="text"]').click();
    env.clock.tick(300);
    deepEq(env.cmds('project.new')[2].params, { kind: 'text', pair: 'en-la', text: 'The girl sees the rose.\n\nThe wolf runs.' });
    eq(W.VP_Store.get('project').kind, 'text');
    eq(W.VP_Store.get('project').cues, 2);
    eq(env.q('.vp-cl-title').textContent, 'Paragraphs');
    W.VP_Workspace.close();
    env.clock.tick(300);
    var html = env.document.documentElement;
    var files = { types: ['Files'], files: [{ name: 'lesson-4.vtt' }] };
    env.fire(html, 'dragenter', { dataTransfer: files });
    ok(env.q('.vp-start').classList.contains('vp-dragging'));
    ok(!env.q('.vp-drop-overlay').hidden);
    var allowed = env.fire(html, 'dragover', { dataTransfer: files });
    eq(allowed, false, 'dragover is accepted (default prevented)');
    env.fire(html, 'drop', { dataTransfer: files });
    ok(env.q('.vp-drop-overlay') === null || env.q('.vp-drop-overlay').hidden);
    env.clock.tick(300);
    eq(env.cmds('project.new')[3].params.sourcePath, 'lesson-4.vtt');
    eq(W.VP_Router.current(), 'workspace');
    W.VP_Workspace.close();
    env.clock.tick(300);
    env.window.VP_Bridge.inject({ event: 'dialog.droppedFiles', paths: ['D:\\clase\\fabula.ass'] });
    env.clock.tick(300);
    eq(env.cmds('project.new')[4].params.sourcePath, 'D:\\clase\\fabula.ass');
    W.VP_Workspace.close();
    env.clock.tick(300);
    env.fire(html, 'drop', { dataTransfer: { types: ['Files'], files: [{ name: 'notes.docx' }] } });
    env.clock.tick(100);
    ok(env.q('.vp-toast-text').textContent.indexOf('This file type is not supported') === 0);
    env.q('#vp-start-orberg').checked = true;
    env.fire(env.q('#vp-start-orberg'), 'change');
    ok(env.q('#vp-start-pair').disabled);
    env.q('[data-start-action="sample"]').click();
    env.clock.tick(300);
    eq(env.cmds('project.new')[5].params.pair, 'la-la');
    eq(W.VP_Workspace.mode(), 'orberg', 'opens on the Orbergise tab');
    deepEq(env.errors(), []);
  });

  it('50 remounts and the window drop listeners leave nothing behind', function () {
    var env = boot(seedRecent(3));
    var W = env.window;
    var before = W.VP_Router.counts();
    var nodes = W.VP_Debug.stats().domNodes;
    for (var i = 0; i < 50; i++) { W.VP_Router.go('start'); }
    deepEq(W.VP_Router.counts(), before);
    eq(W.VP_Debug.stats().domNodes, nodes);
    deepEq(W.VP_Debug.failures(), []);
    W.VP_Router.stop();
    env.fire(env.document.documentElement, 'drop', { dataTransfer: { types: ['Files'], files: [{ name: 'x.srt' }] } });
    env.clock.tick(100);
    eq(env.cmds('project.new').length, 0, 'no drop handler after destroy');
  });

  // B8: the real engine's hello (engine/cli/README.md), as captured from vpengine serve.
  function realHello(extra) {
    var h = {
      version: '0.1.0', engine: 'rules-1', engineKind: 'rules', dataDir: '/data', threads: 4,
      lexicons: [{ lang: 'la', available: true, version: '1.0', lemmas: 65315, path: '/data/latin.vpl', tiers: { t1: 511, t2: 3256, t3: 59312 } }],
      model: { available: false, reason: 'not_built', rerankEnabled: false, cpuOk: false },
      modes: ['R', 'O'], online: { allowed: false, mock: false, mockCalls: 0 },
      pairs: ['en-la', 'es-la', 'la-en', 'la-es'],
      pairsUnavailable: [
        { pair: 'en-grc', code: 'bad_params', message: 'only translation into Latin is implemented', hint: 'This language pair is not available yet.' },
        { pair: 'es-grc', code: 'bad_params', message: 'only translation into Latin is implemented', hint: 'This language pair is not available yet.' },
        { pair: 'grc-en', code: 'bad_params', message: 'only translation into Latin is implemented', hint: 'This language pair is not available yet.' },
        { pair: 'grc-es', code: 'bad_params', message: 'only translation into Latin is implemented', hint: 'This language pair is not available yet.' },
        { pair: 'la-la', code: 'bad_params', message: 'source must be English or Spanish', hint: 'This language pair is not available yet.' }
      ],
      samples: [{ lang: 'en', path: '/data/samples/sample.en.srt' }, { lang: 'la', path: '/data/samples/sample.la.srt' }]
    };
    for (var k in extra || {}) { if (Object.prototype.hasOwnProperty.call(extra, k)) { h[k] = extra[k]; } }
    return h;
  }

  it('B8 pair picker from hello.pairs / pairsUnavailable: reasons grouped in the note, Orbergise off with the reason, a missing NLP model names its files', function () {
    var env = boot();
    var W = env.window;
    var S = W.VP_Start;
    var vals = function () { return env.document.querySelectorAll('#vp-start-pair option').map(function (o) { return o.disabled; }); };
    // before the engine answers (or an engine without `pairs`): the B6 table
    W.VP_Store.set('engine', { state: 'connecting' });
    deepEq(vals(), [false, false, false, false, true, true, true, true]);
    eq(env.document.querySelectorAll('#vp-start-pair option')[4].textContent, 'English → Ancient Greek (coming later)');
    eq(env.q('#vp-start-pair-note').hidden, true, 'no note before the engine said why');
    W.VP_Store.set('engine', { state: 'ready', kind: 'webview', hello: realHello() });
    deepEq(vals(), [false, false, false, false, true, true, true, true]);
    eq(env.q('#vp-start-orberg').disabled, true, 'la-la is not offered');
    eq(env.q('.vp-start-check').getAttribute('title'), 'This language pair is not available yet.');
    var note = env.document.querySelectorAll('#vp-start-pair-note li');
    eq(note.length, 1, 'one reason, one line');
    eq(note[0].textContent, 'English → Ancient Greek, Spanish → Ancient Greek, Ancient Greek → English, Ancient Greek → Spanish, Latin → simpler Latin: This language pair is not available yet.');
    deepEq(S.pairInfo('en-grc'), { pair: 'en-grc', available: false, known: true, code: 'bad_params', hint: 'This language pair is not available yet.', message: 'only translation into Latin is implemented' });
    eq(S.pairInfo('la-en').available, true);
    // no English NLP models: en-la goes, its hint names the files; the selection moves on
    var h = realHello({ pairs: ['la-en', 'la-es'] });
    h.pairsUnavailable = h.pairsUnavailable.concat([
      { pair: 'en-la', code: 'not_found', message: 'nlp', hint: 'Install english.tag.vpt and english.dep.vpt in data/nlp.' },
      { pair: 'es-la', code: 'not_found', message: 'nlp', hint: 'Install spanish.tag.vpt and spanish.dep.vpt in data/nlp.' }
    ]);
    W.VP_Store.set('engine', { state: 'ready', kind: 'webview', hello: h });
    deepEq(vals(), [true, true, false, false, true, true, true, true]);
    eq(env.q('#vp-start-pair').value, 'la-en', 'the first pair the engine offers');
    var lines = env.document.querySelectorAll('#vp-start-pair-note li').map(function (li) { return li.textContent; });
    eq(lines.length, 3);
    eq(lines[0], 'English → Latin: Install english.tag.vpt and english.dep.vpt in data/nlp.');
    eq(env.document.querySelectorAll('#vp-start-pair option')[0].getAttribute('title'), 'Install english.tag.vpt and english.dep.vpt in data/nlp.');
    // the language switch rebuilds the labels
    env.q('[data-lang="es-MX"]').click();
    eq(env.document.querySelectorAll('#vp-start-pair option')[0].textContent, 'Inglés → latín (no disponible)');
    deepEq(W.VP_I18n.missing(), []);
  });

  it('B8 samples from hello.samples per source language, the button off without one; the status line names the engine', function () {
    var env = boot();
    var W = env.window;
    W.VP_Store.set('engine', { state: 'ready', kind: 'webview', hello: realHello() });
    W.VP_Store.set('engine', { state: 'ready', kind: 'webview', hello: realHello({ engineKind: 'stub' }) });
    ok(env.q('#vp-start-status').textContent.indexOf('Test engine 0.1.0: it copies the text instead of translating') >= 0, env.q('#vp-start-status').textContent);
    W.VP_Store.set('engine', { state: 'ready', kind: 'webview', hello: realHello() });
    var line = env.q('#vp-start-status').textContent;
    ok(line.indexOf('Translation engine 0.1.0') >= 0, line);
    ok(line.indexOf('This version has no local model') >= 0, line);
    var btn = env.q('[data-start-action="sample"]');
    eq(btn.disabled, false);
    eq(W.VP_Start.samplePath('la-en'), '/data/samples/sample.la.srt');
    eq(W.VP_Start.samplePath('es-la'), null, 'the engine lists no Spanish sample');
    env.q('#vp-start-pair').value = 'es-la';
    env.fire(env.q('#vp-start-pair'), 'change');
    eq(btn.disabled, true);
    eq(btn.getAttribute('title'), 'There is no sample file in Spanish.');
    env.q('#vp-start-pair').value = 'la-en';
    env.fire(env.q('#vp-start-pair'), 'change');
    eq(btn.disabled, false);
    btn.click();
    env.clock.tick(100);
    deepEq(env.cmds('project.new')[0].params, { kind: 'subs', pair: 'la-en', sourcePath: '/data/samples/sample.la.srt' });
  });
});
