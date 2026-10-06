describe('VP_App', function () {
  // These tests exercise the placeholder start screen, so the B6 screens are left out
  // (vp_start.test.js and vp_workspace.test.js boot the app with them).
  var SCREENS = ['vp_start.js', 'vp_workspace.js', 'vp_cuelist.js', 'vp_panes.js'];
  function boot(search, before, opts) {
    opts = opts || {};
    opts.search = search;
    opts.skip = SCREENS.concat(opts.skip || []);
    var env = load('all', opts);
    var D = env.window.VP_Dom;
    env.main = D.el('main', { id: 'vp-main' });
    env.status = D.el('div', { id: 'vp-status' });
    env.document.body.appendChild(env.main);
    env.document.body.appendChild(env.status);
    if (before) { before(env); }
    env.result = env.window.VP_App.boot();
    env.clock.tick(500);
    return env;
  }
  function text(env, sel) { var n = env.document.querySelector(sel); return n ? n.textContent : null; }

  it('boots with the mock: strings, engine status line, status bar, no missing keys', function () {
    var env = boot('?mock=1&debug=1');
    var W = env.window;
    ok(W.VP_App.ready());
    eq(env.document.documentElement.getAttribute('data-vp-ready'), 'true');
    eq(env.document.documentElement.getAttribute('lang'), 'en-US');
    eq(W.VP_Store.get('engine').state, 'ready');
    eq(W.VP_Router.current(), 'start');
    eq(text(env, 'h1'), 'Welcome to vetus poeta');
    var line = text(env, '#vp-engine-line');
    ok(line.indexOf('Practice engine 0.0.0-mock') >= 0, line);
    ok(line.indexOf('Latin dictionary mock-1: 54,199 entries') >= 0, line);
    ok(line.indexOf('Ancient Greek dictionary mock-1: 23,169 entries') >= 0, line);
    ok(line.indexOf('Local model not installed (optional)') >= 0, line);
    var status = text(env, '#vp-status');
    ok(status.indexOf('Offline: nothing leaves this computer') >= 0, status);
    ok(status.indexOf('Translator ready') >= 0, status);
    eq(text(env, '#vp-sample-la'), W.VP_App.fontSample().la);
    eq(env.document.querySelector('#vp-sample-grc').getAttribute('lang'), 'grc');
    eq(env.document.querySelectorAll('.vp-chip').length, 3);
    eq(env.document.querySelectorAll('.vp-tier .vp-leaf').length, 9);
    deepEq(W.VP_I18n.missing(), []);
    eq(env.errors().length, 0);
    eq(env.innerHTMLWrites, 0);
  });

  it('every control has a name, every field a label, ids are unique (structural a11y check)', function () {
    var env = boot('?mock=1');
    var doc = env.document;
    doc.querySelectorAll('button').forEach(function (b) {
      ok(b.textContent.replace(/\s+/g, '') || b.getAttribute('aria-label'), 'button without a name: ' + b.className);
    });
    doc.querySelectorAll('input, textarea, select').forEach(function (f) {
      ok(f.id && doc.querySelector('label[for="' + f.id + '"]'), 'field without a label: ' + f.id);
    });
    doc.querySelectorAll('[role="group"], [role="progressbar"], [role="img"]').forEach(function (g) {
      ok(g.getAttribute('aria-label'), g.getAttribute('role') + ' without aria-label');
    });
    var ids = {};
    doc.querySelectorAll('[id]').forEach(function (n) {
      ok(!ids[n.id], 'duplicate id ' + n.id);
      ids[n.id] = true;
    });
    eq(doc.querySelectorAll('h1').length, 1);
  });

  it('switches language and theme from the placeholder and saves them in settings', function () {
    var env = boot('?mock=1');
    var W = env.window;
    env.document.querySelector('[data-lang="es-MX"]').click();
    eq(env.document.documentElement.getAttribute('lang'), 'es-MX');
    eq(text(env, 'h1'), 'Te damos la bienvenida a vetus poeta');
    ok(text(env, '#vp-engine-line').indexOf('Diccionario de latín mock-1: 54,199 entradas') >= 0, text(env, '#vp-engine-line'));
    ok(text(env, '#vp-status').indexOf('Sin conexión') >= 0);
    eq(env.document.querySelector('[data-lang="es-MX"]').getAttribute('aria-pressed'), 'true');
    eq(env.document.querySelector('[data-lang="en-US"]').getAttribute('aria-pressed'), 'false');
    env.document.querySelector('[data-theme-choice="dark"]').click();
    eq(env.document.documentElement.getAttribute('data-theme'), 'dark');
    eq(env.document.querySelector('[data-theme-choice="dark"]').getAttribute('aria-pressed'), 'true');
    env.clock.tick(100);
    eq(W.VP_Store.get('settings').lang, 'es-MX');
    eq(W.VP_Store.get('settings').theme, 'dark');
    env.document.querySelector('[data-theme-choice="auto"]').click();
    eq(env.document.documentElement.hasAttribute('data-theme'), false);
    deepEq(W.VP_I18n.missing(), []);
  });

  it('applies saved settings: language, theme and text size', function () {
    var env = boot('?mock=1', function (e) {
      e.window.localStorage.setItem('vp.mock.settings', JSON.stringify({ lang: 'es-MX', theme: 'light', textScale: 120 }));
    });
    eq(env.document.documentElement.getAttribute('lang'), 'es-MX');
    eq(env.document.documentElement.getAttribute('data-theme'), 'light');
    eq(env.document.documentElement.style['--text-scale'], '1.2');
  });

  it('F1 opens the shortcut list from VP_Keys; Escape closes it', function () {
    var env = boot('?mock=1');
    var W = env.window;
    env.key(env.document.body, 'F1');
    eq(W.VP_Dialog.count(), 1);
    var dlg = W.VP_Dialog.top().el;
    ok(dlg.querySelectorAll('kbd').length >= 29, 'kbd count');
    ok(dlg.textContent.indexOf('Accept and go to the next cue to review') >= 0);
    env.key(env.document.activeElement, 'Escape');
    eq(W.VP_Dialog.count(), 0);
    deepEq(W.VP_I18n.missing(), []);
  });

  it('the placeholder screen survives 50 remounts with no leak; tour, toast and dialog demos work', function () {
    var env = boot('?mock=1&debug=1');
    var W = env.window;
    var before = W.VP_Router.counts();
    var nodes = W.VP_Debug.stats().domNodes;
    for (var i = 0; i < 50; i++) { W.VP_Router.go('start'); }
    deepEq(W.VP_Router.counts(), before);
    eq(W.VP_Debug.stats().domNodes, nodes);
    deepEq(W.VP_Debug.failures(), []);
    env.document.querySelector('[data-action="tour"]').click();
    ok(W.VP_Tour.isActive());
    eq(text(env, '.vp-tour-title'), 'Language and theme');
    W.VP_Router.go('start');
    eq(W.VP_Tour.isActive(), false);
    deepEq(W.VP_Router.counts(), before);
    env.document.querySelector('[data-action="toast"]').click();
    eq(W.VP_Toast.count(), 1);
    env.document.querySelector('.vp-toast-action').click();
    eq(text(env, '.vp-toast-text'), 'Example change undone.');
    env.document.querySelector('[data-action="dialog"]').click();
    eq(W.VP_Dialog.count(), 1);
    env.document.activeElement.click();
    eq(W.VP_Dialog.count(), 0);
    deepEq(W.VP_Debug.failures(), []);
    eq(env.errors().length, 0);
  });

  it('without a transport it says no translator is connected', function () {
    var env = boot('');
    eq(env.window.VP_Store.get('engine').state, 'none');
    ok(text(env, '#vp-engine-line').indexOf('No translator is connected') >= 0);
    ok(text(env, '#vp-status').indexOf('No translator connected') >= 0);
    ok(env.window.VP_App.ready());
  });

  it('an engine error shows the translated title and hint for its code', function () {
    var env = boot('?mock=1', function (e) {
      e.window.VP_MockEngine.options.failNext = { cmd: 'engine.hello', code: 'lexicon_missing', message: 'x', hint: 'engine hint' };
    });
    var line = text(env, '#vp-engine-line');
    ok(line.indexOf('The dictionary is missing') >= 0, line);
    ok(line.indexOf('Reinstall vetus poeta') >= 0, line);
    var E = env.window.VP_App.errorText('weird_code', 'engine says why');
    eq(E.title, 'Something went wrong');
    eq(E.hint, 'engine says why');
  });

  it('query flags pick language and theme; without Promise it refuses to boot', function () {
    var env = boot('?mock=1&lang=es-MX&theme=dark&reducedMotion=1');
    eq(env.document.documentElement.getAttribute('lang'), 'es-MX');
    eq(env.document.documentElement.getAttribute('data-theme'), 'dark');
    ok(env.document.documentElement.classList.contains('vp-reduced-motion'));
    var bare = boot('?mock=1', null, { noPolyfill: true, skip: ['polyfill_promise.js'] });
    eq(bare.result, null);
    eq(bare.document.documentElement.getAttribute('data-boot-error'), 'promise');
  });

  it('B8 bridge init order: under the WebView2 transport the bridge listener exists before the Start screen mounts, so the router leak check stays clean', function () {
    var env = load('all', { search: '?debug=1', webview: true });
    var W = env.window;
    var D = W.VP_Dom;
    var app = D.el('div', { id: 'app', className: 'vp-app' });
    var main = D.el('main', { id: 'vp-main' });
    app.appendChild(main);
    env.document.body.appendChild(app);
    var mountedWith = null;
    var mount = W.VP_Start.mount;
    W.VP_Start.mount = function (root, params) {
      mountedWith = { bridge: W.VP_Bridge.kind(), listeners: D.count('bridge') };
      return mount(root, params);
    };
    W.VP_App.boot({ root: main, status: D.el('div') });
    env.clock.tick(200);
    deepEq(mountedWith, { bridge: 'webview', listeners: 1 }, 'the WebView2 listener is registered before the first screen');
    // the shell answers like the engine
    var answer = function (cmd, result) {
      env.webviewSent.forEach(function (raw) {
        var m = JSON.parse(raw);
        if (m.cmd === cmd && !m.answered) { env.webviewReply({ id: m.id, ok: true, result: result }); }
      });
      env.clock.tick(200);
    };
    answer('engine.hello', { version: '0.1.0', engine: 'rules-1', engineKind: 'rules', lexicons: [], pairs: ['en-la'], pairsUnavailable: [], samples: [], model: { available: false, rerankEnabled: false } });
    answer('settings.get', { lang: 'en-US', theme: 'light', tourSeenVersion: W.VP_App.TOUR_VERSION });
    ok(W.VP_App.ready());
    eq(W.VP_Store.get('engine').state, 'ready');
    W.VP_Router.go('start');
    env.clock.tick(50);
    var leaks = W.VP_Debug.failures().filter(function (f) { return f.code === 'router.leak'; });
    deepEq(leaks, [], 'no router.leak when the Start screen is left');
    eq(D.count('bridge'), 1, 'the bridge listener stays for the app');
  });
});
