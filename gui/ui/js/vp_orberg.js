/* vp_orberg.js - Orbergise mode (DESIGN 13; PREDESIGN 1.2.3): the centre column of the
 * Orbergise tab. Three stacked panes for the selected cue: Latin file (the cue's source),
 * Original-language file ("not loaded - Choose file..." -> dialog.openFile, then
 * orbergise.start {originalPath}; afterwards cue.get carries `original`), and the Orberg
 * version (word chips; changed words carry an underline and their tier badge, clicking one
 * publishes VP_Store 'inspect' with {orberg:{was, now, why}} so the Word tab shows
 * "was -> now" with the reason; Edit swaps in a textarea kept through cue.set). The "Meaning
 * check" chip shows cue.get `meaning` {percent, missing[]} (content lemmas of the rewrite
 * against the source). Options Tier ceiling T1 | T2, Keep names, Simplify sentence structure
 * are kept in the UI settings key `orberg` and sent with orbergise.start; "Orbergise (all |
 * selected)" runs through VP_Workspace.cmd.orbergise so the job bar and events are shared.
 *
 * VP_Orberg.mount(el) / destroy(); options(); setOption(key, value); chooseOriginal() ->
 * Promise; run(indices?) -> Promise; startEdit() / cancelEdit() / acceptEdit(); changes();
 * meaningChip(meaning) (pure -> {kind, percent, missing}); stats()
 */
(function () {
  'use strict';

  var OWNER = 'orberg';
  var DETAIL_CAP = 20;
  var WORD_RE = /[^\s.,;:?!¿¡"“”«»()\[\]{}\-–—·;]+/g;
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

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function display(text) {
    return settings().showMacrons === false && window.VP_CueList ? window.VP_CueList.stripMacrons(String(text || '')) : String(text || '');
  }

  function lru(cap) {
    var keys = [];
    var map = {};
    return {
      get: function (k) { return Object.prototype.hasOwnProperty.call(map, k) ? map[k] : undefined; },
      put: function (k, v) {
        if (!Object.prototype.hasOwnProperty.call(map, k)) { keys.push(k); }
        map[k] = v;
        while (keys.length > cap) { delete map[keys.shift()]; }
      },
      drop: function (k) {
        if (Object.prototype.hasOwnProperty.call(map, k)) {
          keys.splice(keys.indexOf(k), 1);
          delete map[k];
        }
      },
      size: function () { return keys.length; },
      clear: function () {
        keys = [];
        map = {};
      }
    };
  }

  function options() {
    var o = settings().orberg || {};
    return { tier: o.tier === 1 ? 1 : 2, keepNames: o.keepNames !== false, simplify: o.simplify !== false, originalPath: o.originalPath || '' };
  }

  function setOption(key, value) {
    var o = options();
    o[key] = value;
    var done = window.VP_App && typeof window.VP_App.saveSettings === 'function' ? window.VP_App.saveSettings({ orberg: o }) : P().resolve(null);
    renderOptions();
    return done;
  }

  // The meaning chip: ok at 100 %, check below, fix below 60 % or without data.
  function meaningChip(m) {
    if (!m || typeof m.percent !== 'number') { return { kind: 'none', percent: null, missing: [] }; }
    var pct = Math.max(0, Math.min(100, Math.round(m.percent)));
    return { kind: pct >= 100 ? 'ok' : (pct >= 60 ? 'check' : 'fix'), percent: pct, missing: (m.missing || []).slice() };
  }

  function cue() { return s && s.index !== null ? window.VP_Store.getCue(s.index) : null; }

  // Changed words of the rewrite: {k, was, now, why} from cue.get reasons (data.was/now).
  function changes() {
    var d = s && s.detail;
    var out = [];
    ((d && d.reasons) || []).forEach(function (r) {
      if (r && r.data && r.data.was) { out.push({ k: r.tokenIndex, was: r.data.was, now: r.data.now, why: r.data.why || '', tier: r.data.tier }); }
    });
    return out;
  }

  // ---------------------------------------------------------------- rendering
  function renderOptions() {
    if (!s) { return; }
    var o = options();
    s.tierBtns['1'].setAttribute('aria-pressed', o.tier === 1 ? 'true' : 'false');
    s.tierBtns['2'].setAttribute('aria-pressed', o.tier === 2 ? 'true' : 'false');
    s.keepSw.setAttribute('aria-checked', o.keepNames ? 'true' : 'false');
    s.simplifySw.setAttribute('aria-checked', o.simplify ? 'true' : 'false');
    var job = window.VP_Store.get('job');
    s.runAll.disabled = !!job || !window.VP_Store.cueTotal();
    s.runSel.disabled = !!job || s.index === null;
  }

  function renderOriginal(c) {
    var D = window.VP_Dom;
    D.clear(s.origText);
    var d = s.detail;
    var o = options();
    if (d && typeof d.original === 'string') {
      s.origText.appendChild(el('span', { className: 'vp-text', text: d.original }));
      s.origChoose.hidden = true;
      s.origName.hidden = false;
      s.origName.textContent = String(o.originalPath || '').split(/[\\\/]/).pop();
      return;
    }
    s.origName.hidden = true;
    s.origChoose.hidden = false;
    s.origText.appendChild(i18nEl('span', 'vp-hint', o.originalPath ? 'orbergise.original.pending.label' : 'orbergise.original.none.label'));
  }

  function placeTokens(text, tokens) {
    var out = [];
    var pos = 0;
    for (var i = 0; i < tokens.length; i++) {
      var at = text.indexOf(tokens[i].text, pos);
      if (at < 0) { return null; }
      out.push({ tok: tokens[i], at: at });
      pos = at + tokens[i].text.length;
    }
    return out;
  }

  function localTokens(text) {
    var out = [];
    var re = new RegExp(WORD_RE.source, 'g');
    var m;
    while ((m = re.exec(text)) !== null) { out.push({ tok: { text: m[0], display: m[0] }, at: m.index }); }
    return out;
  }

  function renderVersion(c) {
    var D = window.VP_Dom;
    D.clear(s.verText);
    s.tokens = [];
    var m = meaningChip(s.detail && s.detail.meaning);
    s.chip.className = 'vp-chip vp-orb-chip vp-chip-' + (m.kind === 'none' ? 'check' : m.kind);
    s.chip.hidden = !c || c.state === 'new';
    if (!s.chip.hidden) {
      s.chipText.textContent = m.kind === 'none' ? T('orbergise.meaning.none.label') : T('orbergise.meaning.label', { pct: m.percent });
      s.chip.setAttribute('title', m.missing.length ? T('orbergise.meaning.missing.tooltip', { list: m.missing.join(', ') }) : T('orbergise.meaning.ok.tooltip'));
    }
    D.clear(s.missingEl);
    s.missingEl.hidden = !m.missing.length;
    if (m.missing.length) {
      s.missingEl.appendChild(i18nEl('span', 'vp-hint', 'orbergise.meaning.missing.label'));
      m.missing.forEach(function (w) { s.missingEl.appendChild(el('span', { className: 'vp-text vp-orb-missing', lang: 'la', text: display(w) })); });
    }
    if (!c || !c.target) {
      s.verText.appendChild(i18nEl('span', 'vp-hint', 'orbergise.version.none.label'));
      s.editBtn.disabled = true;
      return;
    }
    s.editBtn.disabled = s.mode === 'edit';
    var text = c.target;
    var toks = (s.detail && s.detail.tokens && s.detail.tokens.length && placeTokens(text, s.detail.tokens)) || localTokens(text);
    var byK = {};
    changes().forEach(function (ch) { byK[ch.k] = ch; });
    var pos = 0;
    toks.forEach(function (t, k) {
      if (t.at > pos) { s.verText.appendChild(document.createTextNode(display(text.slice(pos, t.at)))); }
      var ch = byK[k];
      var attrs = { className: 'vp-word' + (ch ? ' vp-word-changed' : ''), tabIndex: 0, role: 'button', 'data-tok': String(k), text: display(t.tok.display || t.tok.text) };
      if (ch) { attrs.title = T('orbergise.changed.tooltip', { was: ch.was, now: ch.now }); }
      s.verText.appendChild(el('span', attrs));
      if (ch && window.VP_Inspector) {
        var badge = window.VP_Inspector.tierBadge(ch.tier || t.tok.tier, true);
        if (badge) { s.verText.appendChild(badge); }
      }
      s.tokens.push(t.tok);
      pos = t.at + t.tok.text.length;
    });
    if (pos < text.length) { s.verText.appendChild(document.createTextNode(display(text.slice(pos)))); }
  }

  function render() {
    if (!s) { return; }
    var c = cue();
    s.empty.hidden = s.index !== null;
    s.body.hidden = s.index === null;
    renderOptions();
    if (s.index === null) { return; }
    window.VP_Dom.clear(s.srcText);
    if (c) { window.VP_Panes.renderMarkup(s.srcText, c.source); }
    renderOriginal(c);
    if (s.mode !== 'edit') { renderVersion(c); }
  }

  function fetchDetail(index) {
    var gen = s.gen;
    var hit = s.details.get(index);
    if (hit) {
      s.detail = hit;
      render();
      return;
    }
    window.VP_Bridge.call('cue.get', { index: index }).then(function (r) {
      if (!s || s.gen !== gen) { return; }
      s.details.put(index, r);
      if (s.index === index) {
        s.detail = r;
        render();
      }
    }, function () {
      if (s && s.gen === gen && s.index === index) {
        s.detail = { tokens: [], reasons: [] };
        render();
      }
    });
  }

  function show(index) {
    if (!s) { return; }
    if (s.mode === 'edit') { leaveEdit(); }
    s.index = typeof index === 'number' ? index : null;
    s.detail = null;
    s.shown = cue();
    render();
    if (s.index !== null) { fetchDetail(s.index); }
  }

  // ---------------------------------------------------------------- editing
  function startEdit() {
    if (!s || s.index === null || s.mode === 'edit') { return false; }
    var c = cue();
    if (!c) { return false; }
    s.mode = 'edit';
    s.editOrig = c.target || '';
    s.input.value = s.editOrig;
    s.editor.hidden = false;
    s.verText.hidden = true;
    s.editBtn.disabled = true;
    s.input.focus();
    return true;
  }

  function leaveEdit() {
    s.mode = 'view';
    s.editor.hidden = true;
    s.verText.hidden = false;
    s.editBtn.disabled = false;
  }

  function cancelEdit() {
    if (!s || s.mode !== 'edit') { return false; }
    leaveEdit();
    render();
    s.verText.focus();
    return true;
  }

  function acceptEdit() {
    if (!s || s.mode !== 'edit') { return P().resolve(false); }
    var text = String(s.input.value).replace(/\r/g, '');
    var index = s.index;
    if (text === s.editOrig) {
      cancelEdit();
      return P().resolve(false);
    }
    leaveEdit();
    return window.VP_Workspace.cmd.edit(index, text).then(function () {
      if (s) {
        s.details.drop(index);
        if (s.index === index) { fetchDetail(index); }
      }
      return true;
    }, function (err) {
      showError(err);
      return false;
    });
  }

  // ---------------------------------------------------------------- actions
  function chooseOriginal() {
    return window.VP_Bridge.call('dialog.openFile', { kind: 'original', filters: ['srt', 'vtt', 'ass', 'ssa', 'txt'] }).then(function (r) {
      if (!r || !r.path || r.cancelled) { return null; }
      return setOption('originalPath', r.path).then(function () { return run(null); }).then(function () { return r.path; });
    }, function (err) {
      showError(err);
      return null;
    });
  }

  function run(indices) {
    var o = options();
    var params = { tier: o.tier, keepNames: o.keepNames, simplify: o.simplify };
    if (o.originalPath) { params.originalPath = o.originalPath; }
    if (indices) { params.indices = indices; }
    if (s) { s.details.clear(); }
    return window.VP_Workspace.cmd.orbergise(params);
  }

  function openWord(k) {
    if (!s || !s.tokens[k]) { return false; }
    var tok = s.tokens[k];
    var ch = null;
    changes().forEach(function (x) { if (x.k === k) { ch = x; } });
    window.VP_Store.set('inspect', { index: s.index, token: k, text: tok.text, lang: 'la', lemmaId: tok.lemmaId === undefined ? null : tok.lemmaId, orberg: ch });
    return true;
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-orb-action');
    var tier = btn.getAttribute('data-orb-tier');
    if (tier) { setOption('tier', Number(tier)); } else if (a === 'keep') { setOption('keepNames', btn.getAttribute('aria-checked') !== 'true'); } else if (a === 'simplify') {
      setOption('simplify', btn.getAttribute('aria-checked') !== 'true');
    } else if (a === 'choose') { chooseOriginal(); } else if (a === 'runAll') { run(null); } else if (a === 'runSel') {
      if (s.index !== null) { run([s.index]); }
    } else if (a === 'edit') { startEdit(); } else if (a === 'cancel') { cancelEdit(); } else if (a === 'keepEdit') { acceptEdit(); }
  }

  function onVersionClick(e) {
    var word = window.VP_Dom.closest(e.target, '.vp-word', s.verText);
    if (word) { openWord(Number(word.getAttribute('data-tok'))); }
  }

  function onVersionKey(e) {
    var word = window.VP_Dom.closest(e.target, '.vp-word', s.verText);
    if (!word) { return; }
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
      e.preventDefault();
      e.stopPropagation();
      openWord(Number(word.getAttribute('data-tok')));
    }
  }

  function sw(id, labelKey, action) {
    return el('div', { className: 'vp-eng-row' }, [
      i18nEl('span', 'vp-eng-name', labelKey, null, { id: id + '-label' }),
      el('button', { id: id, type: 'button', role: 'switch', className: 'vp-switch', 'aria-checked': 'false', 'aria-labelledby': id + '-label', dataset: { orbAction: action } }, [el('span', { className: 'vp-switch-knob', 'aria-hidden': 'true' })])
    ]);
  }

  function build(root) {
    s.tierBtns = {
      '1': i18nEl('button', 'vp-seg', 'orbergise.tier.t1.label', null, { type: 'button', 'aria-pressed': 'false', dataset: { orbTier: '1' } }),
      '2': i18nEl('button', 'vp-seg', 'orbergise.tier.t2.label', null, { type: 'button', 'aria-pressed': 'false', dataset: { orbTier: '2' } })
    };
    var keepRow = sw('vp-orb-keep', 'orbergise.keepNames.label', 'keep');
    var simplifyRow = sw('vp-orb-simplify', 'orbergise.simplify.label', 'simplify');
    s.keepSw = keepRow.childNodes[1];
    s.simplifySw = simplifyRow.childNodes[1];
    s.runAll = i18nEl('button', 'vp-btn vp-btn-primary', 'orbergise.run.all.cta', null, { type: 'button', id: 'vp-orb-run', dataset: { orbAction: 'runAll' } });
    s.runSel = i18nEl('button', 'vp-btn vp-btn-secondary', 'orbergise.run.selected.cta', null, { type: 'button', dataset: { orbAction: 'runSel' } });
    var src = String((window.VP_Store.get('project') || {}).pair || 'la-la').split('-')[0];
    s.srcText = el('div', { className: 'vp-pane-text vp-text', lang: src });
    s.origText = el('div', { className: 'vp-pane-text vp-orb-orig' });
    s.origChoose = i18nEl('button', 'vp-btn vp-btn-secondary', 'orbergise.original.choose.cta', null, { type: 'button', dataset: { orbAction: 'choose' } });
    s.origName = el('span', { className: 'vp-hint vp-mono', hidden: true });
    s.verText = el('div', { id: 'vp-orb-version', className: 'vp-pane-text vp-target-view vp-text', lang: 'la', tabIndex: -1 });
    s.chipText = el('span');
    s.chip = el('span', { className: 'vp-chip vp-orb-chip', hidden: true }, [el('span', { className: 'vp-chip-shape', 'aria-hidden': 'true' }), s.chipText]);
    s.missingEl = el('p', { className: 'vp-orb-missinglist', hidden: true });
    s.editBtn = i18nEl('button', 'vp-btn vp-btn-secondary', 'target.edit.cta', null, { type: 'button', dataset: { orbAction: 'edit' } });
    s.input = el('textarea', { id: 'vp-orb-editor', className: 'vp-editor-input vp-text vp-orb-input', lang: 'la', rows: '3', spellcheck: 'false' });
    s.editor = el('div', { className: 'vp-editor', hidden: true }, [
      el('label', { htmlFor: 'vp-orb-editor', className: 'vp-visually-hidden', 'data-i18n': 'orbergise.version.title' }),
      s.input,
      el('div', { className: 'vp-row vp-editor-actions' }, [
        i18nEl('button', 'vp-btn vp-btn-secondary', 'target.editor.cancel.cta', null, { type: 'button', dataset: { orbAction: 'cancel' } }),
        i18nEl('button', 'vp-btn vp-btn-primary', 'target.editor.keep.cta', null, { type: 'button', dataset: { orbAction: 'keepEdit' } })
      ])
    ]);
    s.empty = i18nEl('p', 'vp-panes-empty vp-hint', 'target.select.empty');
    s.body = el('div', { className: 'vp-orb-body', hidden: true }, [
      el('section', { className: 'vp-pane', 'aria-labelledby': 'vp-orb-src-title' }, [
        el('div', { className: 'vp-pane-head' }, [src === 'la' ? i18nEl('h2', 'vp-pane-title', 'orbergise.latin.title', null, { id: 'vp-orb-src-title' }) :
          i18nEl('h2', 'vp-pane-title', 'source.pane.title', { lang: window.VP_I18n.has('app.lexicon.lang.' + src) ? T('app.lexicon.lang.' + src) : src }, { id: 'vp-orb-src-title' })]),
        s.srcText
      ]),
      el('section', { className: 'vp-pane', 'aria-labelledby': 'vp-orb-orig-title' }, [
        el('div', { className: 'vp-pane-head' }, [i18nEl('h2', 'vp-pane-title', 'orbergise.original.title', null, { id: 'vp-orb-orig-title' }), s.origName, s.origChoose]),
        s.origText
      ]),
      el('section', { className: 'vp-pane vp-pane-target', 'aria-labelledby': 'vp-orb-ver-title' }, [
        el('div', { className: 'vp-pane-head' }, [i18nEl('h2', 'vp-pane-title', 'orbergise.version.title', null, { id: 'vp-orb-ver-title' }), s.chip]),
        s.verText,
        s.editor,
        s.missingEl,
        el('div', { className: 'vp-row vp-target-actions' }, [s.editBtn])
      ])
    ]);
    s.root = el('div', { className: 'vp-orb' }, [
      el('div', { className: 'vp-card vp-orb-options' }, [
        el('div', { className: 'vp-row vp-orb-optrow' }, [
          el('div', { className: 'vp-orb-tier' }, [
            i18nEl('span', 'vp-eng-name', 'orbergise.tier.label', null, { id: 'vp-orb-tier-label' }),
            el('div', { className: 'vp-segmented', role: 'group', 'aria-labelledby': 'vp-orb-tier-label' }, [s.tierBtns['1'], s.tierBtns['2']])
          ]),
          keepRow, simplifyRow
        ]),
        el('div', { className: 'vp-row' }, [s.runAll, s.runSel]),
        i18nEl('p', 'vp-hint', 'orbergise.options.hint')
      ]),
      s.empty,
      s.body
    ]);
    root.appendChild(s.root);
  }

  function mount(root) {
    if (s) { destroy(); }
    s = { gen: (mount.gen = (mount.gen || 0) + 1), index: null, detail: null, shown: null, tokens: [], mode: 'view', editOrig: '', details: lru(DETAIL_CAP), removers: [] };
    build(root);
    var D = window.VP_Dom;
    D.delegate(s.root, '[data-orb-action], [data-orb-tier]', 'click', onClick, { owner: OWNER });
    D.on(s.verText, 'click', onVersionClick, { owner: OWNER });
    D.on(s.verText, 'keydown', onVersionKey, { owner: OWNER });
    D.on(s.input, 'keydown', function (e) {
      if (e.key === 'Escape' || e.key === 'Esc') {
        e.preventDefault();
        cancelEdit();
      } else if (e.key === 'Enter' && e.ctrlKey) {
        e.preventDefault();
        acceptEdit();
      }
    }, { owner: OWNER });
    var S = window.VP_Store;
    s.removers.push(S.subscribe('selection', function (sel) { show(sel ? sel.index : null); }));
    s.removers.push(S.subscribe('cues', function () {
      if (!s || s.index === null) { return; }
      var c = cue();
      if (c && s.shown && (c.target !== s.shown.target || c.state !== s.shown.state)) {
        s.shown = c;
        s.details.drop(s.index);
        fetchDetail(s.index);
      } else if (c && !s.shown) {
        s.shown = c;
        render();
      }
    }));
    s.removers.push(S.subscribe('settings', render));
    s.removers.push(S.subscribe('job', renderOptions));
    s.removers.push(window.VP_I18n.onLanguageChanged(render));
    s.removers.push(window.VP_Debug.registerCache('orbergCueGet', function () { return s ? s.details.size() : 0; }));
    var sel = S.get('selection');
    show(sel ? sel.index : null);
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    s.details.clear();
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s = null;
  }

  function i18nKeys() {
    return ['orbergise.meaning.label', 'orbergise.meaning.none.label', 'orbergise.meaning.missing.tooltip', 'orbergise.meaning.ok.tooltip', 'orbergise.changed.tooltip', 'orbergise.original.pending.label', 'orbergise.original.none.label'];
  }

  window.VP_Orberg = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    options: options,
    setOption: setOption,
    chooseOriginal: chooseOriginal,
    run: run,
    startEdit: startEdit,
    cancelEdit: cancelEdit,
    acceptEdit: acceptEdit,
    changes: changes,
    meaningChip: meaningChip,
    openWord: openWord,
    stats: function () { return s ? { index: s.index, mode: s.mode, details: s.details.size(), changes: changes().length } : null; },
    i18nKeys: i18nKeys
  };
}());
