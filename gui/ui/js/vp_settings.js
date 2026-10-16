/* vp_settings.js - the Settings page (DESIGN 13; PREDESIGN 1.4): one scrolling dialog with a
 * left anchor list. Every control applies at once through VP_Settings.apply(path, value):
 * the store first (VP_App.saveSettings), then settings.set; there is no Save button.
 * Sections: General (language, theme, text size 90-140, macrons in app, emoji in app, grammar
 * colours), Translation defaults (pair, fidelity, export defaults), Engines (model path,
 * size, SHA-256 check, last load, Test model, Unload now; Online master + Wiktionary +
 * Test), Saving (autosave, Open projects folder -> shell.revealFile dataDir, Recover from last
 * crash -> project.open of the autosave path when one is known), Performance (eco mode, Free
 * memory now -> model.unload), Learning (reading speed adult/child, Reset tour), Reset to
 * defaults (DESIGN 9.1 defaults, one settings.set). Shortcut Ctrl+, (VP_Workspace / VP_App).
 *
 * defaultFidelity is on the engine's scale (rules.h: 1 extremely faithful .. 3 flexible); the
 * select lists 1, 2, 3 in that order. Builds before B9 saved it inverted (3 = faithful):
 * migrate() inverts a saved 1 or 3 once and sets fidelityScaleV2 in the same settings.set
 * (VP_App runs it after settings.get at boot); with the flag present nothing changes.
 *
 * VP_Settings.open() -> dialog handle; close(); isOpen(); apply(path, value) -> Promise;
 * reset() -> Promise; migrationPatch(settings) -> patch | null (pure); migrate() ->
 * Promise(bool); DEFAULTS; SECTIONS
 */
(function () {
  'use strict';

  var OWNER = 'settings';
  var SECTIONS = ['general', 'translation', 'engines', 'saving', 'performance', 'learning'];
  var LANGS = ['en-US', 'es-MX'];
  var THEMES = ['auto', 'light', 'dark'];
  var ENCODINGS = ['utf-8', 'utf-16le', 'windows-1252'];
  var DEFAULTS = {
    lang: 'en-US', theme: 'auto', textScale: 100, showMacrons: true, showEmoji: true, grammarColours: false,
    defaultPair: 'en-la', defaultFidelity: 2, latinity: 'wide',
    'export': { emoji: false, macrons: false, encoding: 'utf-8', bom: false, rebreak: true },
    engines: { model: false, online: false },
    online: { wiktionary: false, latinitium: false },
    modelPath: '', eco: false, autosave: true, cps: { adult: 17, child: 20 }, tourSeenVersion: '',
    fidelityScaleV2: true
  };
  var TOUR_VERSION = '1';
  var d = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }
  function P() { return window.Promise; }
  function settings() { return window.VP_Store.get('settings') || {}; }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function get(path) {
    var v = settings();
    var parts = String(path).split('.');
    for (var i = 0; i < parts.length; i++) {
      if (v === null || v === undefined || typeof v !== 'object') { return undefined; }
      v = v[parts[i]];
    }
    return v;
  }

  // One patch with the whole sub-object for a dotted key ("export.macrons" -> {export:{...}}).
  function patchFor(path, value) {
    var parts = String(path).split('.');
    var patch = {};
    if (parts.length === 1) {
      patch[path] = value;
      return patch;
    }
    var cur = settings()[parts[0]] || {};
    var sub = {};
    for (var k in cur) { if (Object.prototype.hasOwnProperty.call(cur, k)) { sub[k] = cur[k]; } }
    sub[parts[1]] = value;
    patch[parts[0]] = sub;
    return patch;
  }

  function apply(path, value) {
    var patch = patchFor(path, value);
    var done = window.VP_App && typeof window.VP_App.saveSettings === 'function' ? window.VP_App.saveSettings(patch) : P().resolve(null);
    return done.then(function (r) {
      if (d) { render(); }
      return r;
    });
  }

  // One-time move of defaultFidelity to the engine's scale (B9): null when already done.
  function migrationPatch(st) {
    if (!st || st.fidelityScaleV2 === true) { return null; }
    var patch = { fidelityScaleV2: true };
    var f = Number(st.defaultFidelity);
    if (f === 1 || f === 3) { patch.defaultFidelity = 4 - f; }
    return patch;
  }

  // The store first, then settings.set; the engine's answer is not written back, so a boot
  // with dev query flags (theme, lang) is not re-applied by this one-time write.
  function migrate() {
    var cur = window.VP_Store.get('settings');
    var patch = migrationPatch(cur);
    if (!patch) { return P().resolve(false); }
    var next = {};
    var k;
    for (k in cur) { if (Object.prototype.hasOwnProperty.call(cur, k)) { next[k] = cur[k]; } }
    for (k in patch) { if (Object.prototype.hasOwnProperty.call(patch, k)) { next[k] = patch[k]; } }
    window.VP_Store.set('settings', next);
    if (window.VP_Bridge.kind() === 'none') { return P().resolve(true); }
    return window.VP_Bridge.call('settings.set', { patch: patch }).then(function () { return true; }, function () { return false; });
  }

  function reset() {
    var patch = JSON.parse(JSON.stringify(DEFAULTS));
    patch.lang = window.VP_I18n.lang();
    return window.VP_App.saveSettings(patch).then(function (r) {
      if (d) {
        d.model = null;
        render();
        loadModel();
      }
      window.VP_Toast.show({ key: 'settings.reset.done.label', kind: 'success' });
      return r;
    });
  }

  // ---------------------------------------------------------------- controls
  function field(id, labelKey, control, hintKey) {
    return el('div', { className: 'vp-field vp-set-field' }, [
      el('label', { htmlFor: id, 'data-i18n': labelKey, text: T(labelKey) }),
      control,
      hintKey ? i18nEl('p', 'vp-hint', hintKey, null, { id: id + '-hint' }) : null
    ]);
  }

  function select(id, path, values, keyOf) {
    var cur = get(path);
    var node = el('select', { id: id, className: 'vp-input', dataset: { setPath: path } }, values.map(function (v) {
      return el('option', { value: String(v), selected: String(cur) === String(v), 'data-i18n': keyOf(v), text: T(keyOf(v)) });
    }));
    node.value = String(cur);
    return node;
  }

  function toggle(id, labelKey, path, hintKey, invert) {
    var on = invert ? !get(path) : !!get(path);
    return el('div', { className: 'vp-eng-row vp-set-row' }, [
      i18nEl('span', 'vp-eng-name', labelKey, null, { id: id + '-label' }),
      el('button', { id: id, type: 'button', role: 'switch', className: 'vp-switch', 'aria-checked': on ? 'true' : 'false', 'aria-labelledby': id + '-label', 'aria-describedby': hintKey ? id + '-hint' : null, dataset: { setToggle: path, setInvert: invert ? '1' : '0' } }, [el('span', { className: 'vp-switch-knob', 'aria-hidden': 'true' })]),
      hintKey ? i18nEl('p', 'vp-hint vp-set-hint', hintKey, null, { id: id + '-hint' }) : null
    ]);
  }

  function number(id, path, min, max, step, unitKey) {
    var cur = get(path);
    return el('div', { className: 'vp-set-number' }, [
      el('input', { id: id, type: 'number', className: 'vp-input', min: String(min), max: String(max), step: String(step), value: String(cur === undefined ? '' : cur), dataset: { setPath: path, setMin: String(min), setMax: String(max) } }),
      unitKey ? i18nEl('span', 'vp-hint', unitKey) : null
    ]);
  }

  function pairKeys() {
    var P2 = window.VP_Start && window.VP_Start.PAIRS;
    var list = P2 ? P2.map(function (p) { return p.code; }) : ['en-la', 'es-la', 'la-en', 'la-es'];
    return list;
  }

  function section(id, kids) {
    return el('section', { id: 'vp-set-' + id, className: 'vp-set-section', 'aria-labelledby': 'vp-set-' + id + '-title', tabIndex: -1 }, [
      i18nEl('h3', 'vp-set-title', 'settings.' + id + '.title', null, { id: 'vp-set-' + id + '-title' })
    ].concat(kids));
  }

  function modelBlock() {
    var m = d.model;
    var kids = [];
    if (!m) {
      kids.push(i18nEl('p', 'vp-hint', 'settings.engines.model.checking.label'));
    } else if (m.available) {
      kids.push(el('dl', { className: 'vp-set-dl' }, [
        i18nEl('dt', null, 'settings.engines.model.path.label'), el('dd', { className: 'vp-mono vp-set-path', text: m.path || '' }),
        i18nEl('dt', null, 'settings.engines.model.size.label'), el('dd', { text: T('settings.engines.model.size.value', { n: Math.round((m.sizeBytes || 0) / 1048576) }) }),
        i18nEl('dt', null, 'settings.engines.model.sha.label'), i18nEl('dd', null, m.sha256ok ? 'settings.engines.model.sha.ok.label' : 'settings.engines.model.sha.bad.label'),
        i18nEl('dt', null, 'settings.engines.model.load.label'), el('dd', { text: m.lastLoadMs ? T('settings.engines.model.load.value', { ms: m.lastLoadMs }) : T('settings.engines.model.load.never.label') })
      ]));
      kids.push(el('div', { className: 'vp-row' }, [
        i18nEl('button', 'vp-btn vp-btn-secondary', 'settings.engines.model.test.cta', null, { type: 'button', dataset: { setAction: 'testModel' } }),
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'settings.engines.model.unload.cta', null, { type: 'button', disabled: !m.loaded, dataset: { setAction: 'unload' } })
      ]));
    } else {
      kids.push(i18nEl('p', 'vp-hint', 'settings.engines.model.missing.label'));
      kids.push(el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.model.find.cta', null, { type: 'button', dataset: { setAction: 'findModel' } })]));
    }
    return el('div', { className: 'vp-set-model' }, kids);
  }

  function buildPage() {
    var st = settings();
    var hasModel = !!(d.model && d.model.available);
    var engine = window.VP_Store.get('engine') || {};
    var dataDir = (engine.hello && engine.hello.dataDir) || '';
    var project = window.VP_Store.get('project');
    return [
      section('general', [
        field('vp-set-lang', 'settings.general.lang.label', select('vp-set-lang', 'lang', LANGS, function (v) { return 'app.lang.' + (v === 'es-MX' ? 'es' : 'en') + '.label'; })),
        field('vp-set-theme', 'settings.general.theme.label', select('vp-set-theme', 'theme', THEMES, function (v) { return 'app.theme.' + v + '.label'; })),
        field('vp-set-scale', 'settings.general.textScale.label', el('div', { className: 'vp-set-scale' }, [
          el('input', { id: 'vp-set-scale', type: 'range', className: 'vp-slider', min: '90', max: '140', step: '10', value: String(st.textScale || 100), dataset: { setPath: 'textScale' }, 'aria-valuetext': (st.textScale || 100) + '%' }),
          el('span', { className: 'vp-mono vp-set-scale-value', text: (st.textScale || 100) + '%' })
        ]), 'settings.general.textScale.hint'),
        toggle('vp-set-macrons', 'settings.general.macrons.label', 'showMacrons', 'settings.general.macrons.hint'),
        toggle('vp-set-emoji', 'settings.general.emoji.label', 'showEmoji', null),
        toggle('vp-set-colours', 'settings.general.grammarColours.label', 'grammarColours', 'settings.general.grammarColours.hint')
      ]),
      section('translation', [
        field('vp-set-pair', 'settings.translation.pair.label', select('vp-set-pair', 'defaultPair', pairKeys(), function (v) { return window.VP_Start ? window.VP_Start.pairLabelKey(v) : 'start.pair.enLa.label'; })),
        field('vp-set-fidelity', 'settings.translation.fidelity.label', select('vp-set-fidelity', 'defaultFidelity', [1, 2, 3], function (v) { return 'fidelity.stop.f' + v + '.label'; })),
        i18nEl('h4', 'vp-set-sub', 'settings.translation.export.title'),
        toggle('vp-set-exp-emoji', 'export.options.emoji.label', 'export.emoji', 'export.options.emoji.hint'),
        toggle('vp-set-exp-macrons', 'export.options.macrons.label', 'export.macrons', null),
        field('vp-set-exp-enc', 'export.options.encoding.label', select('vp-set-exp-enc', 'export.encoding', ENCODINGS, function (v) { return 'export.encoding.' + v.replace(/-/g, '') + '.label'; })),
        toggle('vp-set-exp-bom', 'export.options.bom.label', 'export.bom', 'export.options.bom.hint'),
        toggle('vp-set-exp-rebreak', 'export.options.rebreak.label', 'export.rebreak', null)
      ]),
      section('engines', [
        i18nEl('h4', 'vp-set-sub', 'engines.model.label'),
        toggle('vp-set-model', 'settings.engines.model.use.label', 'engines.model', hasModel ? null : 'settings.engines.model.missing.label'),
        modelBlock(),
        i18nEl('h4', 'vp-set-sub', 'settings.engines.online.title'),
        toggle('vp-set-online', 'settings.engines.online.master.label', 'engines.online', 'engines.online.hint'),
        toggle('vp-set-wikt', 'settings.engines.online.wiktionary.label', 'online.wiktionary', null),
        el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.online.test.cta', null, { type: 'button', disabled: !(st.engines && st.engines.online), dataset: { setAction: 'testOnline' } })])
      ]),
      section('saving', [
        toggle('vp-set-autosave', 'settings.saving.autosave.label', 'autosave', 'settings.saving.autosave.hint'),
        el('div', { className: 'vp-row' }, [
          i18nEl('button', 'vp-btn vp-btn-secondary', 'settings.saving.folder.cta', null, { type: 'button', disabled: !dataDir, dataset: { setAction: 'folder' } }),
          i18nEl('button', 'vp-btn vp-btn-secondary', 'settings.saving.recover.cta', null, { type: 'button', disabled: !(project && project.autosavePath), dataset: { setAction: 'recover' } })
        ]),
        i18nEl('p', 'vp-hint', 'settings.saving.recover.hint')
      ]),
      section('performance', [
        toggle('vp-set-eco', 'settings.performance.eco.label', 'eco', 'settings.performance.eco.hint'),
        el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-secondary', 'settings.performance.free.cta', null, { type: 'button', dataset: { setAction: 'free' } })]),
        i18nEl('p', 'vp-hint', 'settings.performance.free.hint')
      ]),
      section('learning', [
        field('vp-set-cps-adult', 'settings.learning.cps.adult.label', number('vp-set-cps-adult', 'cps.adult', 5, 60, 1, 'settings.learning.cps.unit.label')),
        field('vp-set-cps-child', 'settings.learning.cps.child.label', number('vp-set-cps-child', 'cps.child', 5, 60, 1, 'settings.learning.cps.unit.label'), 'settings.learning.cps.hint'),
        el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-secondary', 'settings.learning.tour.cta', null, { type: 'button', dataset: { setAction: 'tour' } })])
      ]),
      el('div', { className: 'vp-set-reset' }, [
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'settings.reset.cta', null, { type: 'button', dataset: { setAction: 'reset' } }),
        i18nEl('p', 'vp-hint', 'settings.reset.hint')
      ])
    ];
  }

  function render() {
    if (!d) { return; }
    var D = window.VP_Dom;
    var active = document.activeElement;
    var keep = active && d.page.contains(active) ? active.getAttribute('id') : null;
    var top = d.page.scrollTop;
    D.clear(d.page);
    D.append(d.page, buildPage());
    window.VP_I18n.bind(d.page);
    d.page.scrollTop = top;
    if (keep) {
      var again = D.qs('#' + keep, d.page);
      if (again) { again.focus(); }
    }
  }

  function loadModel() {
    if (!d) { return; }
    var gen = d.gen;
    window.VP_Bridge.call('model.status', {}).then(function (r) {
      if (!d || d.gen !== gen) { return; }
      d.model = r;
      render();
    }, function () {
      if (!d || d.gen !== gen) { return; }
      d.model = { available: false };
      render();
    });
  }

  // ---------------------------------------------------------------- events
  function onChange(e) {
    var t = e.target;
    if (!t || !t.getAttribute) { return; }
    var path = t.getAttribute('data-set-path');
    if (!path) { return; }
    var v = t.value;
    if (t.localName === 'input' && (t.getAttribute('type') === 'number' || t.getAttribute('type') === 'range')) {
      var n = Number(v);
      var min = Number(t.getAttribute('data-set-min') || t.getAttribute('min'));
      var max = Number(t.getAttribute('data-set-max') || t.getAttribute('max'));
      if (isNaN(n)) { return; }
      n = Math.max(min, Math.min(max, Math.round(n)));
      apply(path, n).then(null, showError);
      return;
    }
    if (path === 'defaultFidelity') { v = Number(v); }
    apply(path, v).then(null, showError);
  }

  function onInput(e) {
    var t = e.target;
    if (t && t.getAttribute && t.getAttribute('data-set-path') === 'textScale') {
      var out = window.VP_Dom.qs('.vp-set-scale-value', d.page);
      if (out) { out.textContent = t.value + '%'; }
      t.setAttribute('aria-valuetext', t.value + '%');
      document.documentElement.style.setProperty('--text-scale', String(Number(t.value) / 100));
    }
  }

  function onClick(e, btn) {
    var tog = btn.getAttribute('data-set-toggle');
    var a = btn.getAttribute('data-set-action');
    var anchor = btn.getAttribute('data-set-anchor');
    if (tog) {
      var invert = btn.getAttribute('data-set-invert') === '1';
      var on = btn.getAttribute('aria-checked') === 'true';
      var next = invert ? on : !on;
      if (tog === 'engines.online' && next && window.VP_Engines) {
        window.VP_Engines.setOnline(true).then(null, showError);
        return;
      }
      apply(tog, next).then(null, showError);
    } else if (anchor) {
      var sec = window.VP_Dom.qs('#vp-set-' + anchor, d.page);
      if (sec) {
        if (typeof sec.scrollIntoView === 'function') { sec.scrollIntoView({ block: 'start' }); }
        sec.focus();
      }
    } else if (a === 'testModel') {
      window.VP_Bridge.call('model.test', {}).then(function (r) {
        if (d) { d.model = r; render(); }
        window.VP_Toast.show({ key: 'settings.engines.model.test.ok.label', vars: { ms: (r && r.lastLoadMs) || 0 }, kind: 'success' });
      }, showError);
    } else if (a === 'unload' || a === 'free') {
      window.VP_Bridge.call('model.unload', {}).then(function (r) {
        if (d) { d.model = r; render(); }
        window.VP_Toast.show({ key: 'settings.performance.free.done.label', kind: 'success' });
      }, showError);
    } else if (a === 'findModel') {
      if (window.VP_Engines) { window.VP_Engines.findModel().then(function () { loadModel(); }); }
    } else if (a === 'testOnline') {
      if (window.VP_Engines) { window.VP_Engines.testOnline(); }
    } else if (a === 'folder') {
      var engine = window.VP_Store.get('engine') || {};
      window.VP_Bridge.call('shell.revealFile', { path: (engine.hello && engine.hello.dataDir) || '' }).then(null, showError);
    } else if (a === 'recover') {
      var p = window.VP_Store.get('project');
      if (p && p.autosavePath) {
        window.VP_Bridge.call('project.recover', { path: p.path || p.autosavePath }).then(function () {
          window.VP_Toast.show({ key: 'settings.saving.recover.done.label', kind: 'success' });
          if (window.VP_Workspace && window.VP_Workspace.isMounted()) { window.VP_Workspace.cmd.refetch([0]); }
        }, showError);
      }
    } else if (a === 'tour') {
      apply('tourSeenVersion', '').then(function () {
        close();
        if (window.VP_App && typeof window.VP_App.startTour === 'function') { window.VP_App.startTour(); }
      });
    } else if (a === 'reset') {
      window.VP_Dialog.confirm({ titleKey: 'settings.reset.title', textKey: 'settings.reset.text', confirmKey: 'settings.reset.confirm' }).then(function (ok) {
        if (ok) { reset().then(null, showError); }
      });
    }
  }

  // ---------------------------------------------------------------- open / close
  function open() {
    if (d) { return d.handle; }
    var D = window.VP_Dom;
    d = { gen: (open.gen = (open.gen || 0) + 1), model: null, removers: [], handle: null };
    d.page = el('div', { className: 'vp-set-page' });
    var nav = el('nav', { className: 'vp-set-nav', 'data-i18n-aria': 'settings.nav.aria', 'aria-label': T('settings.nav.aria') }, [
      el('ul', null, SECTIONS.map(function (id) {
        return el('li', null, [i18nEl('button', 'vp-btn vp-btn-tertiary vp-set-anchor', 'settings.' + id + '.title', null, { type: 'button', dataset: { setAnchor: id } })]);
      }))
    ]);
    var body = el('div', { className: 'vp-set' }, [nav, d.page]);
    D.append(d.page, buildPage());
    D.on(body, 'change', onChange, { owner: OWNER });
    D.on(body, 'input', onInput, { owner: OWNER });
    D.delegate(body, 'button', 'click', onClick, { owner: OWNER });
    d.removers.push(window.VP_Store.subscribe('settings', function () { if (d) { render(); } }));
    d.removers.push(window.VP_I18n.onLanguageChanged(function () { if (d) { render(); } }));
    d.handle = window.VP_Dialog.open({
      titleKey: 'settings.dialog.title', body: body, className: 'vp-dialog-wide vp-dialog-settings',
      actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }],
      initialFocus: '.vp-set-anchor',
      onClose: function () { teardown(); }
    });
    loadModel();
    return d.handle;
  }

  function teardown() {
    if (!d) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (d.removers.length) { d.removers.pop()(); }
    d = null;
  }

  function close() {
    if (!d) { return false; }
    var h = d.handle;
    teardown();
    if (h) { h.close(true); }
    return true;
  }

  function i18nKeys() {
    var keys = ['settings.engines.model.size.value', 'settings.engines.model.load.value', 'settings.engines.model.test.ok.label', 'settings.performance.free.done.label', 'settings.saving.recover.done.label', 'settings.reset.done.label', 'app.lang.en.label', 'app.lang.es.label'];
    SECTIONS.forEach(function (x) { keys.push('settings.' + x + '.title'); });
    THEMES.forEach(function (x) { keys.push('app.theme.' + x + '.label'); });
    ENCODINGS.forEach(function (x) { keys.push('export.encoding.' + x.replace(/-/g, '') + '.label'); });
    return keys;
  }

  window.VP_Settings = {
    open: open,
    close: close,
    isOpen: function () { return d !== null; },
    apply: apply,
    reset: reset,
    migrationPatch: migrationPatch,
    migrate: migrate,
    patchFor: patchFor,
    DEFAULTS: DEFAULTS,
    SECTIONS: SECTIONS.slice(),
    TOUR_VERSION: TOUR_VERSION,
    i18nKeys: i18nKeys
  };
}());
