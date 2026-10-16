/* vp_app.js - boot (DESIGN 13): Promise present, string tables loaded over XHR, bridge
 * connected (WebView2, or the mock with ?mock=1), engine.hello, settings applied (theme,
 * language, text size), router started on the start screen.
 *
 * Screens: "start" (VP_Start, or the placeholder below when vp_start.js is absent) and
 * "workspace" (VP_Workspace). The router follows VP_Store 'project': a project object routes
 * to the workspace, null routes back to the start screen (deferred by one tick so a screen
 * never destroys itself inside its own handler). The placeholder (engine status line, EN/ES
 * and theme switches, the bundled-font sample, the shared components) stays available as
 * VP_App.placeholder for the smoke test's font checks.
 * index.html marks #app with data-autoboot="true"; tests call VP_App.boot({root, status}).
 *
 * Query flags (dev): mock=1, debug=1, lang=en-US|es-MX, theme=light|dark|auto,
 * reducedMotion=1.
 * VP_App.boot(opts) -> Promise; ready(); setLang(code) -> Promise; setTheme(theme);
 * errorText(code, hint) -> {title, hint}; showError(err); flags(); fontSample();
 * saveSettings(patch) -> Promise(settings) (store first, then settings.set)
 * B9: after settings.get the boot runs VP_Settings.migrate() (one-time fidelity scale move).
 * B7: every change of VP_Store 'settings' is applied to the page (theme, text scale,
 * language) so the Settings page and the Engines tab need no extra wiring; openHelp() (the ?
 * menu: shortcuts, tour, about); startTour() with the six steps of PREDESIGN 1.6 (shown once
 * per TOUR_VERSION after boot, the last step offers the sample project); lockup() builds the
 * SVG wordmark (light and dark files, aria-label "vetus poeta").
 */
(function () {
  'use strict';

  var LANGS = ['en-US', 'es-MX'];
  var THEMES = ['auto', 'light', 'dark'];
  // Data, not UI strings: Latin with macrons and breves, polytonic Greek with stacked marks.
  var SAMPLE_LA = 'Mārcus et Iūlia in hortō ambulant; ĕ ŏ ŭ';
  var SAMPLE_GRC = 'ἄνθρωπος ἀγαθός · ᾰ̓́ ῐ̔͂ ᾱ̀ ῠ́';
  var SAMPLE_EMOJI = '🌹 🐺 📜';
  var TOUR_VERSION = '1';
  // PREDESIGN 1.6: six steps, each fixed to a real control when that screen is up (a missing
  // target gives a centred card), the last one offers the sample project.
  var TOUR_STEPS = [
    { target: '.vp-drop-zone', titleKey: 'tour.step1.title', textKey: 'tour.step1.text' },
    { target: '#vp-start-pair', titleKey: 'tour.step2.title', textKey: 'tour.step2.text' },
    { target: '#vp-ptab-engines', titleKey: 'tour.step3.title', textKey: 'tour.step3.text' },
    { target: '#vp-cl-filter', titleKey: 'tour.step4.title', textKey: 'tour.step4.text' },
    { target: '.vp-word', titleKey: 'tour.step5.title', textKey: 'tour.step5.text' },
    { target: '#vp-export-btn', titleKey: 'tour.step6.title', textKey: 'tour.step6.text', actionKey: 'tour.sample.cta' }
  ];

  var booted = false;
  var isReady = false;
  var bootPromise = null;
  var flagSet = {};
  var statusEl = null;

  function parseFlags(search) {
    var out = {};
    String(search || '').replace(/^\?/, '').split('&').forEach(function (pair) {
      if (!pair) { return; }
      var kv = pair.split('=');
      out[decodeURIComponent(kv[0])] = kv.length > 1 ? decodeURIComponent(kv[1]) : '1';
    });
    return {
      mock: out.mock === '1',
      debug: out.debug === '1',
      lang: LANGS.indexOf(out.lang) >= 0 ? out.lang : null,
      theme: THEMES.indexOf(out.theme) >= 0 ? out.theme : null,
      reducedMotion: out.reducedMotion === '1',
      tour: out.tour === '1'
    };
  }

  function loadJson(url) {
    return new window.Promise(function (resolve, reject) {
      var xhr = new window.XMLHttpRequest();
      xhr.open('GET', url, true);
      if (xhr.overrideMimeType) { xhr.overrideMimeType('application/json'); }
      xhr.onload = function () {
        if (xhr.status !== 200 && xhr.status !== 0) {
          reject(new Error(url + ': HTTP ' + xhr.status));
          return;
        }
        try {
          resolve(JSON.parse(xhr.responseText));
        } catch (e) {
          reject(new Error(url + ': ' + e.message));
        }
      };
      xhr.onerror = function () { reject(new Error(url + ': cannot load')); };
      xhr.send();
    });
  }

  function errorText(code, hint) {
    var T = window.VP_I18n;
    var c = T.has('error.' + code + '.title') ? code : 'internal';
    var useOwn = T.has('error.' + code + '.hint');
    return { title: T.t('error.' + c + '.title'), hint: useOwn ? T.t('error.' + code + '.hint') : (hint || T.t('error.internal.hint')) };
  }

  function showError(err) {
    var e = errorText(err && err.code, err && err.hint);
    return window.VP_Toast.show({ text: e.title + ' ' + e.hint, kind: 'error' });
  }

  function persist(patch) {
    if (window.VP_Bridge.kind() === 'none') { return window.Promise.resolve(window.VP_Store.get('settings') || null); }
    return window.VP_Bridge.call('settings.set', { patch: patch }).then(function (s) {
      window.VP_Store.set('settings', s);
      return s;
    }, function (err) {
      if (window.console) { window.console.warn('[VP_App] settings.set failed: ' + err.code); }
      throw err;
    });
  }

  // Settings the UI changes (view toggles, default pair, recent details): the store first so
  // the screen reacts at once, then the engine, whose answer becomes the store value.
  function saveSettings(patch) {
    var cur = window.VP_Store.get('settings') || {};
    var next = {};
    var k;
    for (k in cur) { if (Object.prototype.hasOwnProperty.call(cur, k)) { next[k] = cur[k]; } }
    for (k in patch) { if (Object.prototype.hasOwnProperty.call(patch, k)) { next[k] = patch[k]; } }
    window.VP_Store.set('settings', next);
    return persist(patch).then(null, function () { return null; });
  }

  // ---------------------------------------------------------------- routing
  function route(project) {
    var want = project ? 'workspace' : 'start';
    if (!window.VP_Router.has(want)) { return; }
    window.VP_Timers.setTimeout('app', function () {
      var cur = !!window.VP_Store.get('project');
      var name = cur ? 'workspace' : 'start';
      if (window.VP_Router.current() !== name && window.VP_Router.has(name)) {
        // Notices belong to the screen that showed them (an Undo after closing a project
        // would act on nothing), so they go with it.
        window.VP_Toast.clearAll();
        window.VP_Router.go(name);
      }
    }, 0);
  }

  function setTheme(theme, save) {
    if (THEMES.indexOf(theme) < 0) { theme = 'auto'; }
    var html = document.documentElement;
    if (theme === 'auto') { html.removeAttribute('data-theme'); } else { html.setAttribute('data-theme', theme); }
    html.setAttribute('data-theme-pref', theme);
    if (save !== false) { persist({ theme: theme }).then(null, function () { return null; }); }
    return theme;
  }

  function setLang(code, save) {
    var ok = window.VP_I18n.setLang(code);
    if (ok && save !== false) { persist({ lang: code }).then(null, function () { return null; }); }
    return ok;
  }

  function initialLang() {
    if (flagSet.lang) { return flagSet.lang; }
    var nav = String((window.navigator && window.navigator.language) || '').toLowerCase();
    return nav.indexOf('es') === 0 ? 'es-MX' : 'en-US';
  }

  var applied = { theme: null, textScale: null, lang: null, first: false };

  // The page follows the settings in the store: theme, text size and language (first at boot
  // with the query flags winning, then after every change, from wherever it came).
  function applySettings(s, first) {
    if (!s) { return; }
    var theme = first && flagSet.theme ? flagSet.theme : (s.theme || 'auto');
    if (theme !== applied.theme) {
      applied.theme = theme;
      setTheme(theme, false);
    }
    var scale = typeof s.textScale === 'number' && s.textScale >= 90 && s.textScale <= 140 ? s.textScale : 100;
    if (scale !== applied.textScale) {
      applied.textScale = scale;
      document.documentElement.style.setProperty('--text-scale', String(scale / 100));
    }
    applied.first = true;
    var lang = first && flagSet.lang ? flagSet.lang : s.lang;
    if (LANGS.indexOf(lang) >= 0 && lang !== applied.lang) {
      applied.lang = lang;
      if (lang !== window.VP_I18n.lang()) { window.VP_I18n.setLang(lang); }
    }
  }

  // The SVG lockup (assets/logo_wordmark*.svg copied to img/): one file per theme, the CSS
  // shows the right one; screen readers get the name.
  function lockup() {
    var D = window.VP_Dom;
    return D.el('span', { className: 'vp-wordmark vp-lockup', role: 'img', 'data-i18n-aria': 'app.name', 'aria-label': window.VP_I18n.t('app.name') }, [
      D.el('img', { className: 'vp-wordmark-svg vp-lockup-light', src: 'img/logo_wordmark.svg', alt: '' }),
      D.el('img', { className: 'vp-wordmark-svg vp-lockup-dark', src: 'img/logo_wordmark_dark.svg', alt: '' })
    ]);
  }

  // ---------------------------------------------------------------- help menu and tour
  function openHelp() {
    var D = window.VP_Dom;
    var body = D.el('div', { className: 'vp-helpmenu' }, [
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'app.help.keys.cta', dataset: { helpAction: 'keys' } }),
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'app.help.tour.cta', dataset: { helpAction: 'tour' } }),
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'app.help.about.cta', dataset: { helpAction: 'about' } })
    ]);
    window.VP_I18n.bind(body);
    var handle = window.VP_Dialog.open({ titleKey: 'app.help.title', body: body, actions: [{ labelKey: 'dialog.close.cta', value: false, kind: 'tertiary' }], initialFocus: '[data-help-action="keys"]' });
    D.delegate(body, '[data-help-action]', 'click', function (e, btn) {
      var a = btn.getAttribute('data-help-action');
      handle.close(a);
      if (a === 'keys') { openShortcuts(); } else if (a === 'tour') { startTour(); } else if (a === 'about' && window.VP_About) { window.VP_About.open(); }
    }, { owner: handle.owner });
    return handle;
  }

  function tourSeen() {
    saveSettings({ tourSeenVersion: TOUR_VERSION }).then(null, function () { return null; });
  }

  function startTour() {
    window.VP_Tour.start(TOUR_STEPS, {
      onDone: tourSeen,
      onAction: function () {
        tourSeen();
        if (window.VP_Start && typeof window.VP_Start.openSample === 'function') {
          if (window.VP_Router.current() === 'start') { window.VP_Start.openSample(); } else { window.VP_Toast.show({ key: 'tour.sample.openFirst.label' }); }
        }
      }
    });
  }

  // First run: the tour starts by itself (PREDESIGN 1.6). In mock mode only with ?tour=1, so
  // browser sessions and tests are not covered by the overlay on every reload.
  function maybeTour(settings) {
    if (!settings || settings.tourSeenVersion === TOUR_VERSION || window.VP_Router.current() !== 'start' || !window.VP_Start) { return; }
    if (flagSet.mock && !flagSet.tour) { return; }
    window.VP_Timers.setTimeout('app', function () {
      if (window.VP_Router.current() === 'start' && !window.VP_Tour.isActive() && !window.VP_Dialog.count()) { startTour(); }
    }, 400);
  }

  // ---------------------------------------------------------------- status bar (app level)
  function renderStatus() {
    if (!statusEl) { return; }
    var D = window.VP_Dom;
    var settings = window.VP_Store.get('settings') || {};
    var online = !!(settings.engines && settings.engines.online);
    var engine = window.VP_Store.get('engine') || { state: 'connecting' };
    var engineKey = {
      connecting: 'app.status.engine.connecting.label',
      ready: 'app.status.engine.ready.label',
      failed: 'app.status.engine.failed.label',
      none: 'app.status.engine.none.label'
    }[engine.state] || 'app.status.engine.connecting.label';
    D.clear(statusEl);
    D.append(statusEl, [
      D.el('span', { className: 'vp-net ' + (online ? 'vp-net-online' : 'vp-net-offline') }, [
        D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' }, [D.el('use', { href: online ? '#vp-i-online' : '#vp-i-offline' })]),
        D.el('span', { 'data-i18n': online ? 'app.network.online.label' : 'app.network.offline.label' })
      ]),
      D.el('span', { className: 'vp-status-engine vp-state-' + engine.state, 'data-i18n': engineKey })
    ]);
    window.VP_I18n.bind(statusEl);
  }

  // ---------------------------------------------------------------- shortcuts dialog
  function openShortcuts() {
    var D = window.VP_Dom;
    var K = window.VP_Keys;
    var rows = K.list();
    var body = D.el('div', { className: 'vp-keys' }, K.groups().map(function (g) {
      return D.el('section', { className: 'vp-keys-group' }, [
        D.el('h3', { 'data-i18n': 'key.group.' + g + '.title' }),
        D.el('table', { className: 'vp-keys-table' }, [D.el('tbody', null, rows.filter(function (r) { return r.group === g; }).map(function (r) {
          return D.el('tr', null, [
            D.el('td', { className: 'vp-keys-combo' }, r.keys.map(function (combo) { return D.el('kbd', { text: K.display(combo) }); })),
            D.el('td', { 'data-i18n': r.labelKey })
          ]);
        }))])
      ]);
    }));
    window.VP_I18n.bind(body);
    return window.VP_Dialog.open({ titleKey: 'dialog.keys.title', body: body, className: 'vp-dialog-wide', actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }] });
  }

  // ---------------------------------------------------------------- placeholder start screen
  var placeholder = (function () {
    var OWNER = 'screen:start';
    var removers = [];
    var section = null;
    var line = null;

    function langButtons() {
      var cur = window.VP_I18n.lang();
      window.VP_Dom.qsa('[data-lang]', section).forEach(function (b) {
        b.setAttribute('aria-pressed', b.getAttribute('data-lang') === cur ? 'true' : 'false');
      });
      var theme = document.documentElement.getAttribute('data-theme-pref') || 'auto';
      window.VP_Dom.qsa('[data-theme-choice]', section).forEach(function (b) {
        b.setAttribute('aria-pressed', b.getAttribute('data-theme-choice') === theme ? 'true' : 'false');
      });
    }

    function renderEngine() {
      var D = window.VP_Dom;
      var T = window.VP_I18n;
      var engine = window.VP_Store.get('engine') || { state: 'connecting' };
      var parts = [];
      if (engine.state === 'ready') {
        var hello = engine.hello || {};
        parts.push(D.el('span', { 'data-i18n': engine.kind === 'mock' ? 'app.engine.mock.label' : 'app.engine.ready.label', 'data-i18n-vars': { version: hello.version || '?' } }));
        (hello.lexicons || []).forEach(function (lx) {
          var name = T.has('app.lexicon.lang.' + lx.lang) ? T.t('app.lexicon.lang.' + lx.lang) : lx.lang;
          parts.push(D.el('span', { 'data-i18n': 'app.lexicon', 'data-i18n-vars': { lang: name, version: lx.version, n: lx.lemmas } }));
        });
        if (!(hello.lexicons || []).length) { parts.push(D.el('span', { 'data-i18n': 'app.lexicon.none.label' })); }
        parts.push(D.el('span', { 'data-i18n': hello.model && hello.model.available ? 'app.model.ready.label' : 'app.model.missing.label' }));
      } else if (engine.state === 'failed') {
        var e = errorText(engine.code, engine.hint);
        parts.push(D.el('span', { 'data-i18n': 'app.engine.failed.label' }));
        parts.push(D.el('span', { text: e.title }));
        parts.push(D.el('span', { text: e.hint }));
      } else if (engine.state === 'none') {
        parts.push(D.el('span', { 'data-i18n': 'app.engine.none.label' }));
      } else {
        parts.push(D.el('span', { 'data-i18n': 'app.engine.connecting.label' }));
      }
      D.clear(line);
      D.append(line, parts);
      line.setAttribute('data-state', engine.state);
      T.bind(line);
    }

    function chip(level) {
      var D = window.VP_Dom;
      return D.el('span', { className: 'vp-chip vp-chip-' + level, 'data-i18n-title': 'confidence.' + level + '.tooltip' }, [
        D.el('span', { className: 'vp-chip-shape', 'aria-hidden': 'true' }),
        D.el('span', { 'data-i18n': 'confidence.' + level + '.label' })
      ]);
    }

    function tierBadge(tier) {
      var D = window.VP_Dom;
      var leaves = [];
      for (var i = 1; i <= 3; i++) {
        leaves.push(D.el('svg', { className: 'vp-leaf' + (i <= tier ? ' vp-leaf-on' : ''), 'aria-hidden': 'true', focusable: 'false', viewBox: '0 0 16 16' }, [D.el('use', { href: '#vp-i-leaf' })]));
      }
      return D.el('span', { className: 'vp-tier vp-tier-t' + tier, role: 'img', 'data-i18n-aria': 'tier.t' + tier + '.aria' }, [
        D.el('span', { className: 'vp-tier-leaves' }, leaves),
        D.el('span', { className: 'vp-tier-label', 'data-i18n': 'tier.t' + tier + '.label' })
      ]);
    }

    function segmented(id, ariaKey, attr, items) {
      var D = window.VP_Dom;
      return D.el('div', { id: id, className: 'vp-segmented', role: 'group', 'data-i18n-aria': ariaKey }, items.map(function (it) {
        var a = { type: 'button', className: 'vp-seg', 'aria-pressed': 'false', 'data-i18n': it.key };
        a[attr] = it.value;
        if (it.lang) { a.lang = it.lang; }
        return D.el('button', a);
      }));
    }

    function startTour() {
      window.VP_Tour.start([
        { target: '#vp-start-lang', titleKey: 'tour.demo.lang.title', textKey: 'tour.demo.lang.text' },
        { target: '#vp-engine-line', titleKey: 'tour.demo.engine.title', textKey: 'tour.demo.engine.text' },
        { target: '#vp-start-actions', titleKey: 'tour.demo.help.title', textKey: 'tour.demo.help.text' }
      ]);
    }

    function onClick(e, btn) {
      var lang = btn.getAttribute('data-lang');
      var theme = btn.getAttribute('data-theme-choice');
      var action = btn.getAttribute('data-action');
      if (lang) {
        setLang(lang);
      } else if (theme) {
        setTheme(theme);
        langButtons();
      } else if (action === 'tour') {
        startTour();
      } else if (action === 'keys') {
        openShortcuts();
      } else if (action === 'toast') {
        window.VP_Toast.undoable('app.placeholder.toast.label', null, function () {
          window.VP_Toast.show({ key: 'app.placeholder.toast.undone' });
        });
      } else if (action === 'dialog') {
        window.VP_Dialog.confirm({ titleKey: 'app.placeholder.dialog.title', textKey: 'app.placeholder.dialog.text', confirmKey: 'app.placeholder.dialog.confirm' });
      }
    }

    function mount(root) {
      var D = window.VP_Dom;
      line = D.el('p', { id: 'vp-engine-line', className: 'vp-status-line', 'aria-live': 'polite' });
      section = D.el('section', { className: 'vp-placeholder', 'aria-labelledby': 'vp-start-title' }, [
        D.el('div', { className: 'vp-placeholder-top' }, [
          D.el('h1', { id: 'vp-start-title', tabIndex: -1, 'data-i18n': 'app.placeholder.title' }),
          D.el('div', { className: 'vp-switches' }, [
            segmented('vp-start-lang', 'app.lang.switch.aria', 'data-lang', [
              { key: 'app.lang.en.label', value: 'en-US', lang: 'en' },
              { key: 'app.lang.es.label', value: 'es-MX', lang: 'es' }
            ]),
            segmented('vp-start-theme', 'app.theme.switch.aria', 'data-theme-choice', [
              { key: 'app.theme.auto.label', value: 'auto' },
              { key: 'app.theme.light.label', value: 'light' },
              { key: 'app.theme.dark.label', value: 'dark' }
            ])
          ])
        ]),
        D.el('p', { className: 'vp-lead', 'data-i18n': 'app.placeholder.lead' }),
        D.el('div', { className: 'vp-card' }, [
          D.el('h2', { 'data-i18n': 'app.placeholder.engine.title' }),
          line
        ]),
        D.el('div', { className: 'vp-card' }, [
          D.el('h2', { 'data-i18n': 'app.placeholder.sample.title' }),
          D.el('p', { className: 'vp-hint', 'data-i18n': 'app.placeholder.sample.hint' }),
          D.el('p', { id: 'vp-sample-la', className: 'vp-text vp-sample', lang: 'la', text: SAMPLE_LA }),
          D.el('p', { id: 'vp-sample-grc', className: 'vp-text vp-sample', lang: 'grc', text: SAMPLE_GRC }),
          D.el('p', { className: 'vp-text vp-sample' }, [D.el('span', { id: 'vp-sample-emoji', className: 'vp-emoji', text: SAMPLE_EMOJI })])
        ]),
        D.el('div', { className: 'vp-card' }, [
          D.el('h2', { 'data-i18n': 'app.placeholder.controls.title' }),
          D.el('div', { className: 'vp-row' }, [chip('ok'), chip('check'), chip('fix')]),
          D.el('div', { className: 'vp-row' }, [tierBadge(1), tierBadge(2), tierBadge(3)]),
          D.el('div', { className: 'vp-progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': '40', 'data-i18n-aria': 'app.placeholder.progress.aria' }, [
            D.el('div', { className: 'vp-progress-bar', style: { width: '40%' } })
          ]),
          D.el('div', { className: 'vp-field' }, [
            D.el('label', { htmlFor: 'vp-start-input', 'data-i18n': 'app.placeholder.input.label' }),
            D.el('input', { id: 'vp-start-input', type: 'text', className: 'vp-input', 'data-i18n-placeholder': 'app.placeholder.input.placeholder' })
          ]),
          D.el('div', { id: 'vp-start-actions', className: 'vp-row vp-actions' }, [
            D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', 'data-action': 'tour', 'data-i18n': 'app.tour.cta' }),
            D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-action': 'keys', 'data-i18n': 'app.help.cta' }),
            D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-action': 'dialog', 'data-i18n': 'app.placeholder.dialog.cta' }),
            D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-action': 'toast', 'data-i18n': 'app.placeholder.toast.cta' })
          ])
        ])
      ]);
      root.appendChild(section);
      D.delegate(section, 'button', 'click', onClick, { owner: OWNER });
      removers.push(window.VP_Store.subscribe('engine', renderEngine));
      removers.push(window.VP_I18n.onLanguageChanged(function () {
        renderEngine();
        langButtons();
      }));
      renderEngine();
      langButtons();
      window.VP_I18n.bind(section);
    }

    function destroy() {
      window.VP_Dom.offOwner(OWNER);
      window.VP_Timers.clearAll(OWNER);
      while (removers.length) { removers.pop()(); }
      if (window.VP_Tour.isActive()) { window.VP_Tour.stop(); }
      section = null;
      line = null;
    }

    return { mount: mount, destroy: destroy };
  }());

  // ---------------------------------------------------------------- boot
  function boot(opts) {
    if (booted) { return bootPromise; }
    booted = true;
    opts = opts || {};
    flagSet = parseFlags(window.location && window.location.search);
    if (flagSet.debug) { window.VP_Debug.enable(true); }
    var html = document.documentElement;
    if (flagSet.reducedMotion) { html.classList.add('vp-reduced-motion'); }
    if (typeof window.Promise !== 'function') {
      html.setAttribute('data-boot-error', 'promise');
      return null;
    }
    var root = opts.root || document.getElementById('vp-main');
    statusEl = opts.status || document.getElementById('vp-status');
    setTheme(flagSet.theme || 'auto', false);
    window.VP_Store.set('engine', { state: 'connecting' });
    window.VP_Keys.bind(document);
    window.VP_Keys.handle('help', function () { openShortcuts(); }, 'app');
    window.VP_Keys.handle('settings', function () { if (window.VP_Settings) { window.VP_Settings.open(); } }, 'app');
    window.VP_History.onTrimmed(function () { window.VP_Toast.show({ key: 'toast.historyTrimmed.label' }); });
    window.VP_Bridge.on('engine.restarted', function () { window.VP_Toast.show({ key: 'toast.engineRestarted.label' }); });
    window.VP_Store.subscribe('engine', renderStatus);
    window.VP_Store.subscribe('settings', renderStatus);
    window.VP_Store.subscribe('settings', function (s) { if (applied.first) { applySettings(s, false); } });
    window.VP_I18n.onLanguageChanged(renderStatus);

    bootPromise = loadJson('i18n/en-US.json').then(function (en) {
      window.VP_I18n.load('en-US', en);
      return loadJson('i18n/es-MX.json');
    }).then(function (es) {
      window.VP_I18n.load('es-MX', es);
    }).then(null, function (err) {
      if (window.console) {
        window.console.error('[VP_App] string tables did not load: ' + err.message +
          (window.location && window.location.protocol === 'file:' ? ' (browsers block file:// requests; run node gui/ui/dev/serve.js)' : ''));
      }
    }).then(function () {
      window.VP_I18n.setLang(initialLang());
      if (!window.VP_Router.has('start')) { window.VP_Router.register('start', window.VP_Start || placeholder); }
      if (!window.VP_Router.has('workspace') && window.VP_Workspace) { window.VP_Router.register('workspace', window.VP_Workspace); }
      window.VP_Store.subscribe('project', route);
      // The bridge first: its WebView2 'message' listener belongs to the app, not to the first
      // screen, so it must exist before the router takes that screen's baseline (B8).
      var kind = window.VP_Bridge.init({ mock: flagSet.mock });
      window.VP_Router.start(root, window.VP_Store.get('project') && window.VP_Router.has('workspace') ? 'workspace' : 'start');
      renderStatus();
      if (kind === 'none') {
        window.VP_Store.set('engine', { state: 'none' });
        return null;
      }
      return window.VP_Bridge.call('engine.hello').then(function (hello) {
        window.VP_Store.set('engine', { state: 'ready', kind: kind, hello: hello });
        return window.VP_Bridge.call('settings.get');
      }).then(function (settings) {
        window.VP_Store.set('settings', settings);
        // B9: a defaultFidelity saved on the old inverted scale is moved once; the store changes
        // at once, settings.set follows without holding the boot.
        if (window.VP_Settings && typeof window.VP_Settings.migrate === 'function') {
          window.VP_Settings.migrate();
          settings = window.VP_Store.get('settings') || settings;
        }
        applySettings(settings, true);
        maybeTour(settings);
      }, function (err) {
        window.VP_Store.set('engine', { state: 'failed', code: err.code, hint: err.hint });
      });
    }).then(function () {
      isReady = true;
      html.setAttribute('data-vp-ready', 'true');
    });
    return bootPromise;
  }

  window.VP_App = {
    boot: boot,
    ready: function () { return isReady; },
    setLang: setLang,
    setTheme: setTheme,
    errorText: errorText,
    showError: showError,
    saveSettings: saveSettings,
    openShortcuts: openShortcuts,
    openHelp: openHelp,
    startTour: startTour,
    lockup: lockup,
    TOUR_VERSION: TOUR_VERSION,
    tourSteps: function () { return TOUR_STEPS.slice(); },
    placeholder: placeholder,
    flags: function () { return flagSet; },
    fontSample: function () { return { la: SAMPLE_LA, grc: SAMPLE_GRC, emoji: SAMPLE_EMOJI }; }
  };

  var appEl = document.getElementById('app');
  if (appEl && appEl.getAttribute('data-autoboot') === 'true') { boot(); }
}());
