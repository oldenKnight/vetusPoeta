describe('VP_Orberg (B10: original language, project.orberg, change list, meaning chip)', function () {
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
    return env;
  }
  function openLatinSample(env) {
    env.q('#vp-start-orberg').checked = true;
    env.fire(env.q('#vp-start-orberg'), 'change');
    env.q('[data-start-action="sample"]').click();
    env.clock.tick(300);
    eq(env.window.VP_Workspace.mode(), 'orberg');
  }
  function lastStart(env) {
    var s = env.cmds('orbergise.start');
    return s.length ? s[s.length - 1].params : null;
  }

  it('orbergReasons (pure): was != now are changes, was == now are kept, notes without data and other kinds skipped', function () {
    var env = boot();
    var O = env.window.VP_Orberg;
    var r = O.orbergReasons([
      { tokenIndex: 0, kind: 'orbergise', text: 'putat -> putat (structure)', data: { was: 'putat', now: 'putat', why: 'structure' } },
      { tokenIndex: 1, kind: 'orbergise', text: 'habitat -> vīvit', data: { was: 'habitat', now: 'vīvit', why: 'word from the original\'s translation' } },
      { tokenIndex: 2, kind: 'orbergise', text: '(new) -> et', data: { was: '', now: 'et', why: 'structure' } },
      { tokenIndex: -1, kind: 'orbergise', text: 'rewritten from the Latin: the original could not be translated' },
      { tokenIndex: 1, kind: 'sense', text: 'x', data: { source: 'lives', sense: 'live' } },
      { tokenIndex: 3, kind: 'candidate', text: 'y', data: { lemmaId: 'vivo', chosen: true } }
    ]);
    deepEq(r.changes.map(function (c) { return [c.k, c.was, c.now]; }), [[1, 'habitat', 'vīvit'], [2, '', 'et']]);
    deepEq(r.kept.map(function (c) { return [c.k, c.was, c.now]; }), [[0, 'putat', 'putat']]);
    eq(r.changes[0].why, 'word from the original\'s translation');
    deepEq(O.orbergReasons(null), { changes: [], kept: [] });
    eq(env.errors().length, 0);
  });

  it('the original file: Detect omits originalLang and shows the detected language; a chosen language is sent; Forget sends originalPath ""', function () {
    var env = boot();
    var W = env.window;
    var O = W.VP_Orberg;
    openLatinSample(env);
    eq(O.original(), null);
    var sel = env.q('#vp-orb-lang');
    ok(sel && !env.q('.vp-orb-langwrap').hidden, 'language selector next to Choose file…');
    eq(sel.value, '', 'default: detect');
    deepEq(sel.querySelectorAll('option').map(function (o) { return o.textContent; }), ['Detect language', 'English', 'Spanish']);
    eq(env.q('[data-orb-action="forget"]').hidden, true);
    // Detect
    env.q('[data-orb-action="choose"]').click();
    env.clock.tick(3000);
    var p = lastStart(env);
    eq(p.originalPath, 'C:\\Users\\Teacher\\Videos\\lesson-3.en.srt');
    eq(Object.prototype.hasOwnProperty.call(p, 'originalLang'), false, 'detect = no originalLang');
    deepEq(O.original(), { path: 'C:\\Users\\Teacher\\Videos\\lesson-3.en.srt', name: 'lesson-3.en.srt', lang: 'en', detected: true });
    W.VP_Toast.clearAll();
    W.VP_CueList.select(0);
    env.clock.tick(100);
    eq(env.q('.vp-orb-file').textContent, 'lesson-3.en.srt · English (detected)');
    eq(env.q('.vp-orb-file').hidden, false);
    eq(env.q('.vp-orb-orig').textContent, 'The girl sees the rose.');
    eq(env.q('.vp-orb-orig .vp-text').getAttribute('lang'), 'en');
    eq(env.q('[data-orb-action="choose"]').hidden, true);
    eq(env.q('.vp-orb-langwrap').hidden, true, 'the language belongs to the loaded file now');
    eq(env.q('[data-orb-action="forget"]').hidden, false);
    // a later run keeps the loaded file: neither originalPath nor originalLang
    env.q('#vp-orb-run').click();
    env.clock.tick(3000);
    p = lastStart(env);
    eq(p.originalPath, undefined);
    eq(p.originalLang, undefined);
    eq(O.original().name, 'lesson-3.en.srt', 'still loaded');
    // Forget
    env.q('[data-orb-action="forget"]').click();
    env.clock.tick(3000);
    p = lastStart(env);
    eq(p.originalPath, '', 'Forget sends originalPath ""');
    eq(p.originalLang, undefined);
    eq(O.original(), null);
    eq(W.VP_Store.get('project').orberg.originalPath, null);
    W.VP_Toast.clearAll();
    env.clock.tick(100);
    eq(env.q('.vp-orb-file').hidden, true);
    eq(env.q('[data-orb-action="choose"]').hidden, false);
    ok(env.q('.vp-orb-orig').textContent.indexOf('not loaded') >= 0, env.q('.vp-orb-orig').textContent);
    // Spanish chosen by the user
    sel.value = 'es';
    env.fire(sel, 'change');
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').orberg.originalLang, 'es');
    eq(O.options().originalLang, 'es');
    env.q('[data-orb-action="choose"]').click();
    env.clock.tick(3000);
    p = lastStart(env);
    eq(p.originalLang, 'es');
    eq(O.original().detected, false);
    W.VP_Toast.clearAll();
    W.VP_CueList.select(1);
    env.clock.tick(100);
    eq(env.q('.vp-orb-file').textContent, 'lesson-3.en.srt · Spanish', 'chosen, not detected');
    eq(env.q('.vp-orb-orig').textContent, 'El marinero vive en la isla.');
    // switching the UI language rewords the file line
    W.VP_I18n.setLang('es-MX');
    env.clock.tick(50);
    eq(env.q('.vp-orb-file').textContent, 'lesson-3.en.srt · Español');
    eq(env.q('[data-orb-action="forget"]').textContent, 'Olvidar');
    W.VP_I18n.setLang('en-US');
    deepEq(W.VP_I18n.missing(), []);
    W.VP_Workspace.close();
    env.clock.tick(300);
    deepEq(W.VP_Debug.failures(), []);
    eq(env.errors().length, 0, JSON.stringify(env.errors()));
  });

  it('reopening a project restores project.orberg; the change list hides was == now and counts it; the meaning chip and the original pane come from cue.get', function () {
    var env = boot();
    var W = env.window;
    var O = W.VP_Orberg;
    deepEq(W.VP_Start.normalizeProject({ orberg: { originalPath: null, originalLang: 'en' } }).orberg, { originalPath: null, originalLang: null, detected: false });
    W.VP_Start.openPath('C:\\Users\\Teacher\\Documents\\orberg-lesson.vpoeta');
    env.clock.tick(500);
    eq(W.VP_Router.current(), 'workspace');
    eq(W.VP_Store.get('project').pair, 'la-la');
    eq(W.VP_Workspace.mode(), 'orberg');
    deepEq(W.VP_Store.get('project').orberg, { originalPath: 'C:\\Users\\Teacher\\Videos\\lesson-3.es.srt', originalLang: 'es', detected: false });
    W.VP_CueList.select(0);
    env.clock.tick(100);
    eq(env.q('.vp-orb-file').textContent, 'lesson-3.es.srt · Spanish');
    eq(env.q('.vp-orb-file').getAttribute('title'), 'C:\\Users\\Teacher\\Videos\\lesson-3.es.srt');
    eq(env.q('[data-orb-action="forget"]').hidden, false);
    eq(env.q('.vp-orb-orig').textContent, 'La niña ve la rosa.');
    // cue 0: videt -> spectat changed, rosam -> rosam "structure kept"
    eq(W.VP_Store.getCue(0).target, 'Puella rosam spectat.');
    var items = env.qa('.vp-orb-changes .vp-orb-change');
    eq(items.length, 1, 'only real changes are listed');
    eq(items[0].textContent, 'videt → spectat');
    eq(items[0].getAttribute('aria-label'), 'Was videt, now spectat');
    items.forEach(function (b) { ok(b.querySelector('.vp-orb-was').textContent !== b.querySelector('.vp-orb-now').textContent, 'no identical pair'); });
    eq(env.qa('#vp-orb-version .vp-word-changed').length, 1);
    eq(env.q('#vp-orb-version .vp-word-changed').textContent, 'spectat');
    var kept = env.q('.vp-orb-kept');
    eq(kept.hidden, false);
    eq(kept.textContent, '1 word kept as it was');
    ok(kept.getAttribute('title').indexOf('rosam') > 0, kept.getAttribute('title'));
    eq(O.kept().length, 1);
    eq(O.stats().changes, 1);
    eq(O.stats().kept, 1);
    // the kept word opens the Word tab without a "Changed by Orbergise" block
    env.q('#vp-orb-version .vp-word[data-tok="1"]').click();
    env.clock.tick(100);
    eq(W.VP_Workspace.tab(), 'word');
    eq(env.q('#vp-panel-body').textContent.indexOf('Changed by Orbergise'), -1, 'rosam -> rosam is not a change');
    // a change in the list opens its word
    items[0].click();
    env.clock.tick(100);
    ok(env.q('#vp-panel-body').textContent.indexOf('Changed by Orbergise') >= 0);
    eq(W.VP_Store.get('inspect').token, 2);
    // meaning chip from cue.get meaning {percent, missing}
    eq(env.q('.vp-orb-chip').hidden, false);
    eq(env.q('.vp-orb-chip').textContent, 'Meaning kept: 100%');
    W.VP_CueList.select(1);
    env.clock.tick(100);
    eq(env.q('.vp-orb-chip').textContent, 'Meaning kept: 67%');
    eq(env.q('.vp-orb-chip').getAttribute('title'), 'Missing from the rewrite: habitō');
    eq(env.q('.vp-orb-kept').hidden, true, 'nothing kept in this cue');
    eq(env.q('.vp-orb-orig').textContent, 'El marinero vive en la isla.');
    // a cue without changes: no list at all
    W.VP_CueList.select(2);
    env.clock.tick(100);
    eq(env.q('.vp-orb-changelist').hidden, true);
    deepEq(W.VP_I18n.missing(), []);
    W.VP_Workspace.close();
    env.clock.tick(300);
    deepEq(W.VP_Debug.failures(), []);
    eq(env.errors().length, 0, JSON.stringify(env.errors()));
  });
});
