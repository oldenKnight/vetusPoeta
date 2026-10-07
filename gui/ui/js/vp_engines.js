/* vp_engines.js - the Engines tab of the right panel (DESIGN 13; PREDESIGN 1.2.2, D9, D12, D13).
 *
 * Rules (dictionary + grammar): always on, shown as a locked switch with the dictionaries of
 * engine.hello. Local model: switch (settings engines.model) with its state from model.status;
 * not installed -> "Install..." (how to get the file) and "Find file..." (dialog.openFile, then
 * model.locate); installed -> file name, size, "loads when needed". Online check: switch
 * (engines.online) with a one-time explanation dialog of what leaves the computer, and "Test
 * connection" (online.test). Fidelity slider, 3 detents on the engine's scale (rules.h,
 * DESIGN 9 and 13): left = "Extremely faithful" = fidelity 1 (tier 3 allowed), middle = 2
 * (tier 2), right = "Flexible" = fidelity 3 (tier 1 with paraphrase); slider position ==
 * fidelity == settings defaultFidelity == translate.start fidelity. A line states the effect
 * in numbers (tier counts of the lexicon when engine.hello has them, the file's share from
 * words.list); the strings are keyed by the fidelity value (fidelity.stop.fN,
 * fidelity.effect.fN). Emoji in the app
 * (showEmoji) and macrons in the exported file (export.macrons). B10 (D18): "Accept medieval
 * and ecclesiastical Latin" (settings latinity: on = "wide", the default and the value when
 * the key is missing, e.g. an engine older than C23; off = "classical"). "Translate again" for the
 * selected cue or all cues. Changing fidelity, latinity or an engine marks translated cues stale
 * through VP_Workspace.cmd.markStale (grey dot); edited and reviewed cues are never touched.
 *
 * B8: engine.hello.model.rerankEnabled false -> "helps with understanding English/Spanish
 * only" under the model switch (the model never picks Latin or Greek words then); reason
 * "not_built" -> this version has no local model (no Install / Find buttons). The online
 * section states whether the check can run: the engine runs it only when engines.online
 * and online.wiktionary are both on (hello.online.allowed at start, the settings after),
 * with a button to the Settings page when the switch is on but Wiktionary is off. The
 * warnings of the last translation (VP_Store 'warnings', from VP_Workspace) are listed
 * under their engine with the engine's hint.
 *
 * VP_Engines.mount(el) / destroy(); setFidelity(1..3); sliderToFidelity(pos) /
 * fidelityToSlider(f) (identity, clamped to 1..3); maxTier(f) (4 - f); setModel(bool) / setOnline(bool) -> Promise;
 * latinity() -> "wide" | "classical"; setLatinity(bool on) -> Promise(value); findModel() -> Promise;
 * testOnline() -> Promise; translateAgain('selected'|'all'); stats()
 */
(function () {
  'use strict';

  var OWNER = 'engines';
  var s = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }
  function P() { return window.Promise; }
  function settings() { return window.VP_Store.get('settings') || {}; }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function setText(node, key, vars) {
    node.setAttribute('data-i18n', key);
    if (vars) { node.setAttribute('data-i18n-vars', JSON.stringify(vars)); } else { node.removeAttribute('data-i18n-vars'); }
    window.VP_I18n.bind(node);
  }

  function save(path, value) {
    return window.VP_Settings.apply(path, value);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  // The slider uses the engine's scale: position 1 (left, "Extremely faithful") = fidelity 1
  // (every word, tier 3 allowed) .. position 3 (right, "Flexible") = fidelity 3 (core words
  // with paraphrase, tier 1). maxTier(f) = the highest lexicon tier the engine may use.
  function clampFidelity(f) { return Math.max(1, Math.min(3, Math.round(Number(f) || 2))); }
  function sliderToFidelity(pos) { return clampFidelity(pos); }
  function fidelityToSlider(f) { return clampFidelity(f); }
  function maxTier(f) { return 4 - clampFidelity(f); }

  function fidelity() {
    var f = settings().defaultFidelity;
    return f >= 1 && f <= 3 ? f : 2;
  }

  // D18: anything but "classical" (missing key, older engine) is the default "wide".
  function latinity() { return settings().latinity === 'classical' ? 'classical' : 'wide'; }

  function sw(id, on, action, opts) {
    opts = opts || {};
    return el('button', { id: id, type: 'button', role: 'switch', className: 'vp-switch', 'aria-checked': on ? 'true' : 'false', 'aria-labelledby': id + '-label', 'aria-describedby': opts.desc || null, disabled: !!opts.disabled, 'aria-disabled': opts.locked ? 'true' : null, 'data-eng-action': action }, [
      el('span', { className: 'vp-switch-knob', 'aria-hidden': 'true' })
    ]);
  }

  function row(id, labelKey, control) {
    return el('div', { className: 'vp-eng-row' }, [i18nEl('span', 'vp-eng-name', labelKey, null, { id: id + '-label' }), control]);
  }

  function lexTiers() {
    var engine = window.VP_Store.get('engine') || {};
    var lex = (engine.hello && engine.hello.lexicons) || [];
    var p = window.VP_Store.get('project') || {};
    var dst = String(p.pair || 'en-la').split('-')[1] || 'la';
    for (var i = 0; i < lex.length; i++) { if (lex[i].lang === dst && lex[i].tiers) { return lex[i].tiers; } }
    return null;
  }

  function effect(f) {
    var t = lexTiers();
    if (t && typeof t.t1 === 'number') {
      var mt = maxTier(f);
      var n = t.t1 + (mt >= 2 ? t.t2 || 0 : 0) + (mt >= 3 ? t.t3 || 0 : 0);
      return { key: 'fidelity.effect.f' + f + '.label', vars: { n: n } };
    }
    return { key: 'fidelity.effect.f' + f + '.fixed', vars: null };
  }

  function renderFidelity() {
    if (!s) { return; }
    var f = fidelity();
    s.slider.value = String(fidelityToSlider(f));
    s.slider.setAttribute('aria-valuetext', T('fidelity.stop.f' + f + '.label'));
    setText(s.stopEl, 'fidelity.stop.f' + f + '.label');
    var e = effect(f);
    setText(s.effectEl, e.key, e.vars);
    var share = s.share;
    s.shareEl.hidden = !share;
    if (share) {
      var mt = maxTier(f);
      var within = (share.t1 || 0) + (mt >= 2 ? share.t2 || 0 : 0) + (mt >= 3 ? share.t3 || 0 : 0) + (share.names || 0);
      setText(s.shareEl, 'fidelity.share.label', { pct: Math.round(within * 100) });
    }
  }

  function helloModel() {
    var engine = window.VP_Store.get('engine') || {};
    return (engine.hello && engine.hello.model) || {};
  }

  // The engine runs the online check only with both switches on (engines.online and the
  // Wiktionary source); hello.online.allowed says the same for the settings at start.
  function onlineState() {
    var st = settings();
    var on = !!(st.engines && st.engines.online);
    var wik = !!(st.online && st.online.wiktionary);
    return { on: on, allowed: on && wik };
  }

  function warningLines(engine) {
    var list = (window.VP_Store.get('warnings') || []).filter(function (x) { return x.engine === engine; });
    return list.map(function (wn) {
      var what = window.VP_Workspace && typeof window.VP_Workspace.warningText === 'function' ? window.VP_Workspace.warningText(wn) : wn.code;
      return el('div', { className: 'vp-eng-warn-box', role: 'status' }, [
        i18nEl('p', 'vp-eng-warn', 'engines.warning.label', { what: what }),
        wn.hint ? el('p', { className: 'vp-hint', text: wn.hint }) : null
      ]);
    });
  }

  function renderModel() {
    if (!s) { return; }
    var D = window.VP_Dom;
    var m = s.model || {};
    var hm = helloModel();
    var on = !!(settings().engines && settings().engines.model);
    s.modelSw.setAttribute('aria-checked', on && m.available ? 'true' : 'false');
    s.modelSw.disabled = !m.available;
    D.clear(s.modelStatus);
    var notBuilt = m.reason === 'not_built' || hm.reason === 'not_built';
    if (!s.modelLoaded) {
      s.modelStatus.appendChild(i18nEl('p', 'vp-hint', 'engines.model.checking.label'));
    } else if (notBuilt && !m.available) {
      s.modelStatus.appendChild(i18nEl('p', 'vp-eng-state', 'engines.model.notBuilt.label'));
    } else if (m.available) {
      var name = String(m.path || '').split(/[\\\/]/).pop();
      var mib = Math.round((m.sizeBytes || 0) / 1048576);
      s.modelStatus.appendChild(i18nEl('p', 'vp-eng-state', 'engines.model.ready.label', { name: name, size: mib }));
      if (m.cpuOk === false) { s.modelStatus.appendChild(i18nEl('p', 'vp-hint vp-eng-warn', 'engines.model.cpu.label')); }
    } else {
      s.modelStatus.appendChild(i18nEl('p', 'vp-eng-state', 'engines.model.missing.label'));
      s.modelStatus.appendChild(el('div', { className: 'vp-row' }, [
        i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.model.install.cta', null, { type: 'button', 'data-eng-action': 'install' }),
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'engines.model.find.cta', null, { type: 'button', 'data-eng-action': 'find' })
      ]));
    }
    var rerank = m.rerankEnabled !== undefined ? m.rerankEnabled : hm.rerankEnabled;
    if (rerank === false && !notBuilt) { s.modelStatus.appendChild(i18nEl('p', 'vp-hint vp-eng-scope', 'engines.model.rerankOff.label')); }
    D.append(s.modelStatus, warningLines('model'));
  }

  function renderOnline() {
    if (!s) { return; }
    var D = window.VP_Dom;
    var o = onlineState();
    D.clear(s.onlineStatus);
    if (o.on) {
      s.onlineStatus.appendChild(i18nEl('p', 'vp-eng-state', o.allowed ? 'engines.online.allowed.label' : 'engines.online.notAllowed.label'));
      if (!o.allowed) { s.onlineStatus.appendChild(i18nEl('button', 'vp-btn vp-btn-tertiary', 'engines.online.settings.cta', null, { type: 'button', 'data-eng-action': 'onlineSettings' })); }
    }
    D.append(s.onlineStatus, warningLines('online'));
  }

  function render() {
    if (!s) { return; }
    var st = settings();
    var en = st.engines || {};
    s.onlineSw.setAttribute('aria-checked', en.online ? 'true' : 'false');
    s.testBtn.disabled = !en.online;
    s.emojiSw.setAttribute('aria-checked', st.showEmoji === false ? 'false' : 'true');
    s.macronSw.setAttribute('aria-checked', st['export'] && st['export'].macrons ? 'true' : 'false');
    s.latinSw.setAttribute('aria-checked', latinity() === 'wide' ? 'true' : 'false');
    var sel = window.VP_Store.get('selection');
    var job = window.VP_Store.get('job');
    s.againSel.disabled = !sel || !!job;
    s.againAll.disabled = !!job || !window.VP_Store.cueTotal();
    s.staleEl.hidden = !s.stale;
    if (s.stale) { setText(s.staleEl, 'engines.stale.label', { n: s.stale }); }
    renderModel();
    renderOnline();
    renderFidelity();
  }

  function markStale() {
    var n = window.VP_Workspace && window.VP_Workspace.cmd.markStale ? window.VP_Workspace.cmd.markStale() : 0;
    if (s) {
      s.stale += n;
      render();
    }
    return n;
  }

  function setFidelity(f) {
    f = clampFidelity(f);
    if (f === fidelity()) {
      renderFidelity();
      return P().resolve(f);
    }
    var done = save('defaultFidelity', f);
    markStale();
    renderFidelity();
    return done.then(function () { return f; });
  }

  function setModel(on) {
    var m = (s && s.model) || {};
    if (on && !m.available) { return P().resolve(false); }
    var done = save('engines.model', !!on);
    markStale();
    return done.then(function () { return !!on; });
  }

  // The engine reads latinity at translate.start, so a change marks translated cues stale.
  function setLatinity(on) {
    var v = on ? 'wide' : 'classical';
    if (v === latinity()) {
      if (s) { render(); }
      return P().resolve(v);
    }
    var done = save('latinity', v);
    markStale();
    return done.then(function () { return v; });
  }

  function explainOnline() {
    return new (P())(function (resolve) {
      var body = el('div', { className: 'vp-help' }, [
        i18nEl('p', null, 'engines.online.explain.text'),
        el('ul', null, [i18nEl('li', null, 'engines.online.explain.sent.label'), i18nEl('li', null, 'engines.online.explain.never.label'), i18nEl('li', null, 'engines.online.explain.off.label')])
      ]);
      window.VP_Dialog.open({
        titleKey: 'engines.online.explain.title', body: body,
        actions: [{ labelKey: 'dialog.cancel.cta', value: false, kind: 'secondary' }, { labelKey: 'engines.online.explain.confirm', value: true, kind: 'primary' }],
        initialFocus: '[data-dialog-action="1"]',
        onClose: function (v) { resolve(v === true); }
      });
    });
  }

  function setOnline(on) {
    var st = settings();
    var go = on && !st.onlineExplained ? explainOnline() : P().resolve(true);
    return go.then(function (ok) {
      if (!ok) { return false; }
      var en = {};
      var cur = st.engines || {};
      for (var k in cur) { if (Object.prototype.hasOwnProperty.call(cur, k)) { en[k] = cur[k]; } }
      en.online = !!on;
      var patch = { engines: en };
      if (on) { patch.onlineExplained = true; }
      var done = window.VP_App.saveSettings(patch);
      markStale();
      return done.then(function () { return !!on; });
    });
  }

  function loadModel() {
    var gen = s.gen;
    return window.VP_Bridge.call('model.status', {}).then(function (r) {
      if (!s || s.gen !== gen) { return; }
      s.model = r;
      s.modelLoaded = true;
      render();
    }, function () {
      if (!s || s.gen !== gen) { return; }
      s.model = { available: false };
      s.modelLoaded = true;
      render();
    });
  }

  function findModel() {
    return window.VP_Bridge.call('dialog.openFile', { kind: 'model', filters: ['gguf'] }).then(function (r) {
      if (!r || !r.path || r.cancelled) { return null; }
      return window.VP_Bridge.call('model.locate', { path: r.path }).then(function (m) {
        if (s) {
          s.model = m;
          s.modelLoaded = true;
          render();
        }
        window.VP_Toast.show({ key: 'engines.model.found.label', kind: 'success' });
        return m;
      });
    }).then(null, function (err) {
      showError(err);
      return null;
    });
  }

  function installHelp() {
    return window.VP_Dialog.open({ titleKey: 'engines.model.install.title', textKey: 'engines.model.install.text', actions: [
      { labelKey: 'dialog.close.cta', value: false, kind: 'secondary' },
      { labelKey: 'engines.model.find.cta', value: true, kind: 'primary', onClick: function () { findModel(); } }
    ] });
  }

  function testOnline() {
    return window.VP_Bridge.call('online.test', {}).then(function (r) {
      window.VP_Toast.show({ key: r && r.ok ? 'engines.online.test.ok.label' : 'engines.online.test.fail.label', vars: { ms: (r && r.latencyMs) || 0 }, kind: r && r.ok ? 'success' : 'error' });
      return r;
    }, function (err) {
      showError(err);
      return null;
    });
  }

  function translateAgain(which) {
    var W = window.VP_Workspace;
    if (!W || !W.isMounted()) { return P().resolve(null); }
    if (which === 'selected') {
      var sel = window.VP_Store.get('selection');
      if (!sel) { return P().resolve(null); }
      return W.cmd.translate([sel.index]);
    }
    if (s) { s.stale = 0; }
    return W.cmd.translate(null);
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-eng-action');
    var st = settings();
    if (a === 'model') {
      setModel(!(st.engines && st.engines.model));
    } else if (a === 'online') {
      setOnline(!(st.engines && st.engines.online));
    } else if (a === 'test') {
      testOnline();
    } else if (a === 'emoji') {
      save('showEmoji', st.showEmoji === false);
    } else if (a === 'latinity') {
      setLatinity(latinity() !== 'wide');
    } else if (a === 'macrons') {
      save('export.macrons', !(st['export'] && st['export'].macrons));
    } else if (a === 'install') {
      installHelp();
    } else if (a === 'find') {
      findModel();
    } else if (a === 'againSel') {
      translateAgain('selected');
    } else if (a === 'againAll') {
      translateAgain('all');
    } else if (a === 'onlineSettings') {
      if (window.VP_Settings) { window.VP_Settings.open(); }
    }
  }

  function lexLines() {
    var engine = window.VP_Store.get('engine') || {};
    var lex = (engine.hello && engine.hello.lexicons) || [];
    var out = [];
    lex.forEach(function (lx) {
      if (lx.available === false || !lx.version) { return; }
      var name = window.VP_I18n.has('app.lexicon.lang.' + lx.lang) ? T('app.lexicon.lang.' + lx.lang) : lx.lang;
      out.push(i18nEl('li', null, 'app.lexicon', { lang: name, version: lx.version, n: lx.lemmas || 0 }));
    });
    if (!out.length) { out.push(i18nEl('li', null, 'app.lexicon.none.label')); }
    return el('ul', { className: 'vp-eng-lex vp-hint' }, out);
  }

  function build(root) {
    var st = settings();
    s.modelSw = sw('vp-eng-model', false, 'model', { desc: 'vp-eng-model-hint' });
    s.onlineSw = sw('vp-eng-online', false, 'online', { desc: 'vp-eng-online-hint' });
    s.emojiSw = sw('vp-eng-emoji', st.showEmoji !== false, 'emoji');
    s.macronSw = sw('vp-eng-macrons', false, 'macrons');
    s.latinSw = sw('vp-eng-latinity', latinity() === 'wide', 'latinity', { desc: 'vp-eng-latinity-hint' });
    s.modelStatus = el('div', { className: 'vp-eng-status', 'aria-live': 'polite' });
    s.onlineStatus = el('div', { className: 'vp-eng-status vp-eng-online-status', 'aria-live': 'polite' });
    s.testBtn = i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.online.test.cta', null, { type: 'button', 'data-eng-action': 'test' });
    s.slider = el('input', { id: 'vp-fidelity', type: 'range', className: 'vp-slider', min: '1', max: '3', step: '1', 'aria-describedby': 'vp-fidelity-effect' });
    s.stopEl = el('p', { className: 'vp-fid-stop', 'aria-hidden': 'true' });
    s.effectEl = el('p', { id: 'vp-fidelity-effect', className: 'vp-hint vp-fid-effect' });
    s.shareEl = el('p', { className: 'vp-hint vp-fid-share', hidden: true });
    s.staleEl = el('p', { className: 'vp-eng-stale', role: 'status', hidden: true });
    s.againSel = i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.again.selected.cta', null, { type: 'button', 'data-eng-action': 'againSel' });
    s.againAll = i18nEl('button', 'vp-btn vp-btn-secondary', 'engines.again.all.cta', null, { type: 'button', 'data-eng-action': 'againAll' });
    s.root = el('div', { className: 'vp-panel vp-panel-engines' }, [
      i18nEl('h2', 'vp-panel-title', 'engines.panel.title'),
      el('section', { className: 'vp-eng' }, [
        row('vp-eng-rules', 'engines.rules.label', sw('vp-eng-rules', true, 'rules', { locked: true, desc: 'vp-eng-rules-hint' })),
        i18nEl('p', 'vp-hint', 'engines.rules.hint', null, { id: 'vp-eng-rules-hint' }),
        lexLines()
      ]),
      el('section', { className: 'vp-eng' }, [
        row('vp-eng-model', 'engines.model.label', s.modelSw),
        s.modelStatus,
        i18nEl('p', 'vp-hint', 'engines.model.hint', null, { id: 'vp-eng-model-hint' })
      ]),
      el('section', { className: 'vp-eng' }, [
        row('vp-eng-online', 'engines.online.label', s.onlineSw),
        i18nEl('p', 'vp-hint', 'engines.online.hint', null, { id: 'vp-eng-online-hint' }),
        s.onlineStatus,
        el('div', { className: 'vp-row' }, [s.testBtn])
      ]),
      el('section', { className: 'vp-eng vp-fid' }, [
        el('h3', null, [el('label', { htmlFor: 'vp-fidelity', 'data-i18n': 'fidelity.title', text: T('fidelity.title') })]),
        el('div', { className: 'vp-fid-scale' }, [i18nEl('span', 'vp-fid-end', 'fidelity.faithful.label'), s.slider, i18nEl('span', 'vp-fid-end', 'fidelity.flexible.label')]),
        s.stopEl, s.effectEl, s.shareEl
      ]),
      el('section', { className: 'vp-eng' }, [
        row('vp-eng-emoji', 'engines.emoji.label', s.emojiSw),
        row('vp-eng-latinity', 'engines.latinity.label', s.latinSw),
        i18nEl('p', 'vp-hint', 'engines.latinity.hint', null, { id: 'vp-eng-latinity-hint' }),
        row('vp-eng-macrons', 'engines.macrons.label', s.macronSw),
        i18nEl('p', 'vp-hint', 'engines.macrons.hint')
      ]),
      el('section', { className: 'vp-eng' }, [
        i18nEl('h3', null, 'engines.again.title'),
        s.staleEl,
        el('div', { className: 'vp-row' }, [s.againSel, s.againAll]),
        i18nEl('p', 'vp-hint', 'engines.again.hint')
      ])
    ]);
    root.appendChild(s.root);
  }

  function loadShare() {
    if (!window.VP_Store.get('project')) { return; }
    var gen = s.gen;
    window.VP_Bridge.call('words.list', { limit: 1 }).then(function (r) {
      if (!s || s.gen !== gen) { return; }
      var t = r && r.tierShare;
      s.share = t && (t.t1 || t.t2 || t.t3 || t.names) ? t : null;
      renderFidelity();
    }, function () { return null; });
  }

  function mount(root) {
    if (s) { destroy(); }
    var D = window.VP_Dom;
    s = { gen: (mount.gen = (mount.gen || 0) + 1), model: null, modelLoaded: false, share: null, stale: 0, removers: [] };
    build(root);
    D.delegate(s.root, '[data-eng-action]', 'click', onClick, { owner: OWNER });
    D.on(s.slider, 'change', function () { setFidelity(sliderToFidelity(s.slider.value)); }, { owner: OWNER });
    D.on(s.slider, 'input', function () {
      var f = sliderToFidelity(s.slider.value);
      setText(s.stopEl, 'fidelity.stop.f' + f + '.label');
      s.slider.setAttribute('aria-valuetext', T('fidelity.stop.f' + f + '.label'));
      var e = effect(f);
      setText(s.effectEl, e.key, e.vars);
    }, { owner: OWNER });
    var S = window.VP_Store;
    s.removers.push(S.subscribe('settings', render));
    s.removers.push(S.subscribe('selection', render));
    s.removers.push(S.subscribe('job', render));
    s.removers.push(S.subscribe('warnings', render));
    s.removers.push(S.subscribe('engine', render));
    s.removers.push(window.VP_I18n.onLanguageChanged(render));
    render();
    loadModel();
    loadShare();
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s = null;
  }

  function i18nKeys() {
    var keys = ['engines.stale.label', 'engines.model.ready.label', 'fidelity.share.label', 'engines.online.test.ok.label', 'engines.online.test.fail.label', 'app.lexicon'];
    [1, 2, 3].forEach(function (f) { keys.push('fidelity.stop.f' + f + '.label', 'fidelity.effect.f' + f + '.label', 'fidelity.effect.f' + f + '.fixed'); });
    return keys;
  }

  window.VP_Engines = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    setFidelity: setFidelity,
    sliderToFidelity: sliderToFidelity,
    fidelityToSlider: fidelityToSlider,
    maxTier: maxTier,
    setModel: setModel,
    setOnline: setOnline,
    latinity: latinity,
    setLatinity: setLatinity,
    findModel: findModel,
    testOnline: testOnline,
    translateAgain: translateAgain,
    stats: function () { return s ? { stale: s.stale, model: s.model, share: s.share } : null; },
    i18nKeys: i18nKeys
  };
}());
