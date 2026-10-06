describe('VP_Settings', function () {
  function boot(before) {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    env.app = D.el('div', { id: 'app', className: 'vp-app' });
    env.main = D.el('main', { id: 'vp-main' });
    env.app.appendChild(env.main);
    env.document.body.appendChild(env.app);
    W.VP_MockEngine.options.latencyMs = 1;
    if (before) { before(env); }
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
  function change(env, sel, value) {
    var el = env.q(sel);
    el.value = value;
    env.fire(el, 'change');
    env.clock.tick(50);
  }

  it('opens from the start screen gear and Ctrl+,; every control applies immediately through settings.set and the page follows (theme, text size, language)', function () {
    var env = boot();
    var W = env.window;
    env.q('[data-start-action="settings"]').click();
    env.clock.tick(50);
    ok(W.VP_Settings.isOpen());
    eq(env.q('.vp-dialog-title').textContent, 'Settings');
    eq(env.qa('.vp-set-anchor').length, 6);
    eq(env.qa('.vp-set-section').length, 6);
    eq(env.document.activeElement, env.q('.vp-set-anchor'));
    env.qa('.vp-set-anchor')[3].click();
    eq(env.document.activeElement, env.q('#vp-set-saving'), 'anchor moves the focus to the section');
    change(env, '#vp-set-theme', 'dark');
    eq(W.VP_Store.get('settings').theme, 'dark');
    eq(env.cmds('settings.set')[0].params.patch.theme, 'dark');
    eq(env.document.documentElement.getAttribute('data-theme'), 'dark');
    change(env, '#vp-set-scale', '120');
    eq(W.VP_Store.get('settings').textScale, 120);
    eq(env.document.documentElement.style['--text-scale'], '1.2');
    eq(env.q('.vp-set-scale-value').textContent, '120%');
    change(env, '#vp-set-scale', '300');
    eq(W.VP_Store.get('settings').textScale, 140, 'clamped to 90..140');
    change(env, '#vp-set-lang', 'es-MX');
    eq(W.VP_I18n.lang(), 'es-MX');
    eq(env.q('.vp-dialog-title').textContent, 'Configuración');
    change(env, '#vp-set-lang', 'en-US');
    env.q('#vp-set-macrons').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').showMacrons, false);
    eq(env.q('#vp-set-macrons').getAttribute('aria-checked'), 'false');
    env.q('#vp-set-exp-bom').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings')['export'].bom, true);
    eq(W.VP_Store.get('settings')['export'].encoding, 'utf-8', 'nested object merged, not replaced');
    change(env, '#vp-set-exp-enc', 'windows-1252');
    eq(W.VP_Store.get('settings')['export'].encoding, 'windows-1252');
    deepEq(env.qa('#vp-set-fidelity option').map(function (o) { return o.value + ' ' + o.textContent; }),
      ['1 Extremely faithful: any word needed', '2 Balanced: common words', '3 Flexible: basic words, may rephrase'], 'engine scale, faithful first');
    change(env, '#vp-set-fidelity', '1');
    eq(W.VP_Store.get('settings').defaultFidelity, 1);
    eq(env.cmds('settings.set').pop().params.patch.defaultFidelity, 1, 'Extremely faithful is saved as engine fidelity 1');
    change(env, '#vp-set-cps-adult', '99');
    eq(W.VP_Store.get('settings').cps.adult, 60, 'clamped to 5..60');
    eq(W.VP_Store.get('settings').cps.child, 20);
    env.q('#vp-set-eco').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').eco, true);
    env.q('[data-set-action="free"]').click();
    env.clock.tick(50);
    eq(env.cmds('model.unload').length, 1);
    ok(env.q('[data-set-action="folder"]'), 'Open projects folder');
    env.q('[data-set-action="folder"]').click();
    env.clock.tick(50);
    eq(env.cmds('shell.revealFile')[0].params.path, 'mock://data');
    W.VP_Toast.clearAll();
    deepEq(W.VP_I18n.missing(), []);
    env.key(env.document.activeElement, 'Escape');
    eq(W.VP_Settings.isOpen(), false);
    eq(W.VP_Dialog.count(), 0);
    env.key(env.document.body, ',', { ctrl: true });
    env.clock.tick(50);
    ok(W.VP_Settings.isOpen(), 'Ctrl+, from the start screen');
    W.VP_Settings.close();
    eq(env.errors().length, 0);
  });

  it('Engines section: model status, Find file, Test model, Unload; online master + wiktionary; Reset to defaults asks first and resets everything but the language', function () {
    var env = boot();
    var W = env.window;
    W.VP_Settings.open();
    env.clock.tick(50);
    ok(env.q('.vp-set-model').textContent.indexOf('No model file is installed') >= 0);
    env.q('[data-set-action="findModel"]').click();
    env.clock.tick(100);
    eq(env.cmds('model.locate').length, 1);
    ok(env.q('.vp-set-path').textContent.indexOf('latin-helper.gguf') > 0);
    ok(env.q('.vp-set-dl').textContent.indexOf('374 MiB') > 0);
    ok(env.q('.vp-set-dl').textContent.indexOf('not loaded yet') > 0);
    ok(env.q('[data-set-action="unload"]').disabled);
    env.q('[data-set-action="testModel"]').click();
    env.clock.tick(50);
    eq(env.cmds('model.test').length, 1);
    ok(env.q('.vp-set-dl').textContent.indexOf('1,840 ms') > 0);
    ok(!env.q('[data-set-action="unload"]').disabled);
    env.q('[data-set-action="unload"]').click();
    env.clock.tick(50);
    eq(env.cmds('model.unload').length, 1);
    ok(env.q('[data-set-action="unload"]').disabled);
    ok(env.q('[data-set-action="testOnline"]').disabled);
    env.q('#vp-set-online').click();
    env.clock.tick(50);
    eq(W.VP_Dialog.top().el.querySelector('.vp-dialog-title').textContent, 'What leaves the computer', 'the one-time explanation also from Settings');
    W.VP_Dialog.top().el.querySelector('[data-dialog-action="1"]').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').engines.online, true);
    ok(!env.q('[data-set-action="testOnline"]').disabled);
    env.q('#vp-set-wikt').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').online.wiktionary, true);
    W.VP_App.setLang('es-MX');
    env.q('[data-set-action="reset"]').click();
    eq(W.VP_Dialog.top().el.querySelector('.vp-dialog-title').textContent, '¿Restablecer toda la configuración?');
    W.VP_Dialog.top().el.querySelector('[data-dialog-action="0"]').click();
    env.clock.tick(50);
    eq(W.VP_Store.get('settings').engines.online, true, 'cancel changes nothing');
    eq(W.VP_Dialog.count(), 1);
    env.q('[data-set-action="reset"]').click();
    W.VP_Dialog.top().el.querySelector('[data-dialog-action="1"]').click();
    env.clock.tick(100);
    var st = W.VP_Store.get('settings');
    eq(st.engines.online, false);
    eq(st.online.wiktionary, false);
    eq(st.eco, false);
    eq(st.defaultFidelity, 2);
    eq(st.lang, 'es-MX', 'the language stays');
    eq(W.VP_I18n.lang(), 'es-MX');
    eq(st.recentProjects.length, 0);
    W.VP_App.setLang('en-US');
    W.VP_Toast.clearAll();
    W.VP_Settings.close();
    eq(W.VP_Dialog.count(), 0);
    eq(env.errors().length, 0);
  });

  it('patchFor builds a whole sub-object for dotted keys; Reset tour clears tourSeenVersion and starts the tour; the first-run tour has six steps and offers the sample', function () {
    var env = boot();
    var W = env.window;
    W.VP_Store.set('settings', { 'export': { emoji: false, macrons: false, encoding: 'utf-8', bom: false, rebreak: true } });
    deepEq(W.VP_Settings.patchFor('export.macrons', true), { 'export': { emoji: false, macrons: true, encoding: 'utf-8', bom: false, rebreak: true } });
    deepEq(W.VP_Settings.patchFor('eco', true), { eco: true });
    W.VP_Store.set('settings', env.window.VP_MockEngine ? W.VP_Store.get('settings') : {});
    W.VP_App.saveSettings({ tourSeenVersion: '1' });
    env.clock.tick(50);
    W.VP_Settings.open();
    env.clock.tick(50);
    env.q('[data-set-action="tour"]').click();
    env.clock.tick(50);
    eq(W.VP_Settings.isOpen(), false);
    ok(W.VP_Tour.isActive(), 'the tour starts');
    eq(W.VP_Store.get('settings').tourSeenVersion, '');
    eq(W.VP_App.tourSteps().length, 6);
    eq(env.q('.vp-tour-title').textContent, 'Open a subtitle file');
    eq(env.q('.vp-tour-spot').hidden, false, 'step 1 spotlights the drop zone');
    for (var i = 0; i < 5; i++) { W.VP_Tour.next(); }
    eq(W.VP_Tour.index(), 5);
    eq(env.q('.vp-tour-title').textContent, 'Export your file');
    var action = env.q('.vp-tour-action');
    eq(action.hidden, false);
    eq(action.textContent, 'Open the sample project');
    action.click();
    env.clock.tick(400);
    eq(W.VP_Tour.isActive(), false);
    eq(W.VP_Router.current(), 'workspace', 'the last step opens the sample');
    eq(W.VP_Store.get('settings').tourSeenVersion, '1');
    W.VP_Workspace.close();
    env.clock.tick(300);
    deepEq(W.VP_Debug.failures(), []);
  });
});

describe('VP_Settings fidelity scale migration (B9)', function () {
  function bootWith(saved) {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    var main = D.el('main', { id: 'vp-main' });
    env.document.body.appendChild(main);
    W.VP_MockEngine.options.latencyMs = 1;
    if (saved) { W.localStorage.setItem('vp.mock.settings', JSON.stringify(saved)); }
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.sets = function () { return env.sent.filter(function (x) { return x.cmd === 'settings.set'; }); };
    W.VP_App.boot({ root: main, status: D.el('div') });
    env.clock.tick(500);
    return env;
  }

  it('migrationPatch is pure: 1 <-> 3 once, 2 kept, nothing when the flag is set', function () {
    var env = load('all', { search: '?mock=1' });
    var S = env.window.VP_Settings;
    deepEq(S.migrationPatch({ defaultFidelity: 3 }), { fidelityScaleV2: true, defaultFidelity: 1 });
    deepEq(S.migrationPatch({ defaultFidelity: 1 }), { fidelityScaleV2: true, defaultFidelity: 3 });
    deepEq(S.migrationPatch({ defaultFidelity: 2 }), { fidelityScaleV2: true });
    deepEq(S.migrationPatch({}), { fidelityScaleV2: true });
    eq(S.migrationPatch({ defaultFidelity: 3, fidelityScaleV2: true }), null);
    eq(S.DEFAULTS.fidelityScaleV2, true, 'Reset to defaults keeps the flag');
  });

  it('a saved "Extremely faithful" (old 3) becomes engine fidelity 1 at boot through one settings.set, and the next boot changes nothing', function () {
    var env = bootWith({ defaultFidelity: 3 });
    var W = env.window;
    var sets = env.sets();
    eq(sets.length, 1, 'one settings.set');
    deepEq(sets[0].params.patch, { fidelityScaleV2: true, defaultFidelity: 1 });
    eq(W.VP_Store.get('settings').defaultFidelity, 1);
    eq(W.VP_Store.get('settings').fidelityScaleV2, true);
    var stored = JSON.parse(W.localStorage.getItem('vp.mock.settings'));
    eq(stored.fidelityScaleV2, true, 'the mock engine keeps the unknown key like the real one');
    eq(stored.defaultFidelity, 1);
    // second boot on the same saved settings: no inversion back
    var env2 = bootWith(stored);
    eq(env2.sets().length, 0, 'the migration runs once');
    eq(env2.window.VP_Store.get('settings').defaultFidelity, 1);
    eq(env.errors().length, 0);
    eq(env2.errors().length, 0);
  });

  it('a saved "Flexible" (old 1) becomes 3; a fresh install only gets the flag', function () {
    var env = bootWith({ defaultFidelity: 1 });
    deepEq(env.sets()[0].params.patch, { fidelityScaleV2: true, defaultFidelity: 3 });
    eq(env.window.VP_Store.get('settings').defaultFidelity, 3);
    var fresh = bootWith(null);
    deepEq(fresh.sets()[0].params.patch, { fidelityScaleV2: true });
    eq(fresh.window.VP_Store.get('settings').defaultFidelity, 2);
  });
});
