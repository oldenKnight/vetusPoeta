/* vp_export.js - the Export dialog (DESIGN 13; PREDESIGN 1.3; D9, D13, D15).
 *
 * Format radios (the original's format on by default; a text project offers .txt only), file
 * name with the target language code inserted before the extension, Choose... (dialog.saveFile),
 * text options (emoji in the file, macrons, Greek polytonic | monotonic for Greek pairs,
 * encoding + BOM, re-break at 42 characters / 2 lines), the checks (cue count, numbering and
 * timing unchanged; fast cues with "Show them"; Fix cues with "Review first" and the explicit
 * tick that allows exporting anyway), a preview of the first 3 cues from export.preview with
 * the options as set, and Export -> export.write (overwrite:true only after a confirm when the
 * engine answered io "file exists"), then a success toast with "Reveal file" (shell.revealFile).
 * Options start from the settings export.* defaults and change only this export.
 *
 * VP_Export.open() -> handle; close(); isOpen(); checks(cues, total, limit) (pure);
 * fileName(name, pair, format) (pure); options(); setOption(key, value); run() -> Promise
 */
(function () {
  'use strict';

  var OWNER = 'export';
  var FORMATS = ['srt', 'vtt', 'ass'];
  var ENCODINGS = ['utf-8', 'utf-16le', 'windows-1252'];
  var PREVIEW_N = 3;
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

  function project() { return window.VP_Store.get('project') || {}; }

  function pairLangs() {
    var parts = String(project().pair || 'en-la').split('-');
    return { src: parts[0], dst: parts[1] || parts[0] };
  }

  function originalFormat() {
    var p = project();
    if (p.kind === 'text') { return 'txt'; }
    var ext = String(p.sourcePath || p.name || '').split('.').pop().toLowerCase();
    if (ext === 'ssa') { return 'ass'; }
    return FORMATS.indexOf(ext) >= 0 ? ext : 'srt';
  }

  // "Alicia.srt" + en-la + srt -> "Alicia.la.srt" (project files lose their .vpoeta).
  function fileName(name, pair, format) {
    var base = String(name || 'subtitles').split(/[\\\/]/).pop().replace(/\.vpoeta$/i, '').replace(/\.(srt|vtt|ass|ssa|txt)$/i, '');
    var dst = String(pair || 'en-la').split('-')[1] || 'la';
    base = base.replace(new RegExp('\\.' + dst + '$'), '');
    return base + '.' + dst + '.' + format;
  }

  function dirOf(path) {
    var m = /^(.*[\\\/])[^\\\/]*$/.exec(String(path || ''));
    return m ? m[1] : '';
  }

  function cpsLimit() {
    var st = settings();
    return st.cps && typeof st.cps.adult === 'number' ? st.cps.adult : 17;
  }

  // The checks of PREDESIGN 1.3 over the cues the store holds.
  function checks(cues, total, limit) {
    var fast = [];
    var fix = [];
    var untranslated = 0;
    var seen = 0;
    for (var i = 0; i < (cues || []).length; i++) {
      var c = cues[i];
      if (!c) { continue; }
      seen++;
      if (c.state === 'new' || !c.target) { untranslated++; }
      if ((c.flags && c.flags.indexOf('cps') >= 0) || (c.cps || 0) > limit) { fast.push(c.index); }
      if (c.state !== 'new' && c.state !== 'reviewed' && c.confidence === 'fix') { fix.push(c.index); }
    }
    return { total: total, seen: seen, fast: fast, fix: fix, untranslated: untranslated, complete: seen === total };
  }

  function storeCues() {
    var total = window.VP_Store.cueTotal();
    var out = [];
    for (var i = 0; i < total; i++) {
      var c = window.VP_Store.getCue(i);
      if (c) { out.push(c); }
    }
    return out;
  }

  // ---------------------------------------------------------------- rendering
  function radio(name, value, labelKey, checked, data, extra) {
    var attrs = { type: 'radio', name: name, value: value, checked: !!checked };
    attrs['data-exp-' + data] = value;
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { attrs[k] = extra[k]; } } }
    return el('label', { className: 'vp-radio' }, [el('input', attrs), i18nEl('span', null, labelKey)]);
  }

  function check(id, labelKey, key, hintKey) {
    return el('div', { className: 'vp-check' }, [
      el('label', { className: 'vp-check-label' }, [
        el('input', { id: id, type: 'checkbox', checked: !!d.opts[key], dataset: { expOpt: key } }),
        i18nEl('span', null, labelKey)
      ]),
      hintKey ? i18nEl('p', 'vp-hint', hintKey) : null
    ]);
  }

  function statusRow(kind, text, actionKey, action) {
    return el('li', { className: 'vp-exp-check vp-exp-k-' + kind }, [
      el('span', { className: 'vp-chip-shape', 'aria-hidden': 'true' }),
      el('span', { className: 'vp-exp-check-text', text: text }),
      actionKey ? i18nEl('button', 'vp-btn vp-btn-tertiary', actionKey, null, { type: 'button', dataset: { expAction: action } }) : null
    ]);
  }

  function renderChecks() {
    var D = window.VP_Dom;
    D.clear(d.checksEl);
    var ck = d.checks;
    d.checksEl.appendChild(statusRow('ok', ck.complete ? T('export.check.cues.label', { n: ck.total }) : T('export.check.cuesPartial.label', { seen: ck.seen, n: ck.total })));
    if (ck.untranslated) { d.checksEl.appendChild(statusRow('check', T('export.check.untranslated.label', { n: ck.untranslated }))); }
    if (ck.fast.length) { d.checksEl.appendChild(statusRow('check', T('export.check.fast.label', { n: ck.fast.length, limit: cpsLimit() }), 'export.check.fast.cta', 'showFast')); }
    if (ck.fix.length) {
      d.checksEl.appendChild(statusRow('fix', T('export.check.fix.label', { n: ck.fix.length }), 'export.check.fix.cta', 'reviewFix'));
      d.checksEl.appendChild(el('li', { className: 'vp-exp-anyway' }, [el('label', { className: 'vp-check-label' }, [
        el('input', { id: 'vp-exp-anyway', type: 'checkbox', checked: !!d.anyway, dataset: { expAnyway: '1' } }),
        el('span', { text: T('export.check.anyway.label', { n: ck.fix.length }) })
      ])]));
    }
    d.exportBtn.disabled = ck.fix.length > 0 && !d.anyway;
  }

  function renderPreview(r) {
    var D = window.VP_Dom;
    var lang = pairLangs().dst;
    D.clear(d.previewEl);
    if (!r) {
      d.previewEl.appendChild(i18nEl('p', 'vp-hint', 'export.preview.loading.label'));
      return;
    }
    var cues = r.cues || [];
    if (!cues.length) {
      d.previewEl.appendChild(i18nEl('p', 'vp-hint', 'export.preview.none.label'));
      return;
    }
    cues.forEach(function (c) {
      d.previewEl.appendChild(el('div', { className: 'vp-preview-screen vp-exp-screen' }, [
        el('div', { className: 'vp-preview-guide vp-text' }, (c.lines || []).map(function (l) {
          return el('div', { className: 'vp-preview-line' + (l.length > 42 ? ' vp-preview-over' : ''), lang: lang, text: l });
        }))
      ]));
    });
  }

  function previewParams(indices) {
    var o = d.opts;
    return { indices: indices, format: o.format, emoji: !!o.emoji, macrons: !!o.macrons, greek: o.greek, encoding: o.encoding, bom: !!o.bom, rebreak: o.rebreak !== false };
  }

  function loadPreview() {
    if (!d) { return; }
    var gen = ++d.previewGen;
    var idx = [];
    var total = window.VP_Store.cueTotal();
    for (var i = 0; i < total && idx.length < PREVIEW_N; i++) { idx.push(i); }
    if (!idx.length) {
      renderPreview({ cues: [] });
      return;
    }
    renderPreview(null);
    window.VP_Bridge.call('export.preview', previewParams(idx)).then(function (r) {
      if (d && d.previewGen === gen) { renderPreview(r); }
    }, function () {
      if (d && d.previewGen === gen) { renderPreview({ cues: [] }); }
    });
  }

  // d.name is what the field shows; a chosen path (d.path) wins over the typed name.
  function renderName() {
    if (d.path) { d.name = String(d.path).split(/[\\\/]/).pop(); }
    if (!d.name) { d.name = fileName(project().name, project().pair, d.opts.format); }
    if (d.nameEl.value !== d.name) { d.nameEl.value = d.name; }
    d.pathEl.textContent = d.path || (d.dir ? d.dir + d.name : '');
    d.pathEl.hidden = !d.pathEl.textContent;
  }

  function build() {
    var p = project();
    var greek = pairLangs().dst === 'grc';
    var orig = originalFormat();
    var isText = p.kind === 'text';
    var formats = isText ? ['txt'] : FORMATS;
    d.nameEl = el('input', { id: 'vp-exp-name', type: 'text', className: 'vp-input vp-mono', spellcheck: 'false', dataset: { expName: '1' } });
    d.pathEl = el('p', { className: 'vp-hint vp-mono vp-exp-path', hidden: true });
    d.checksEl = el('ul', { className: 'vp-exp-checks' });
    d.previewEl = el('div', { className: 'vp-exp-preview' });
    d.exportBtn = i18nEl('button', 'vp-btn vp-btn-primary', 'export.run.cta', null, { type: 'button', dataset: { expAction: 'run' } });
    d.encEl = el('select', { id: 'vp-exp-enc', className: 'vp-input', dataset: { expSelect: 'encoding' } }, ENCODINGS.map(function (e) {
      return el('option', { value: e, selected: e === d.opts.encoding, 'data-i18n': 'export.encoding.' + e.replace(/-/g, '') + '.label', text: T('export.encoding.' + e.replace(/-/g, '') + '.label') });
    }));
    d.encEl.value = d.opts.encoding;
    var body = el('div', { className: 'vp-exp' }, [
      el('fieldset', { className: 'vp-exp-formats' }, [
        i18nEl('legend', null, 'export.format.label')
      ].concat(formats.map(function (f) { return radio('vp-exp-format', f, 'export.format.' + f + '.label', f === d.opts.format, 'format'); })),
      [orig !== 'txt' ? i18nEl('p', 'vp-hint', 'export.format.hint', { format: '.' + orig }) : null]),
      el('div', { className: 'vp-field' }, [
        el('label', { htmlFor: 'vp-exp-name', 'data-i18n': 'export.name.label', text: T('export.name.label') }),
        el('div', { className: 'vp-row vp-exp-namerow' }, [d.nameEl, i18nEl('button', 'vp-btn vp-btn-secondary', 'export.choose.cta', null, { type: 'button', dataset: { expAction: 'choose' } })]),
        d.pathEl
      ]),
      el('fieldset', { className: 'vp-exp-options' }, [
        i18nEl('legend', null, 'export.options.title'),
        check('vp-exp-emoji', 'export.options.emoji.label', 'emoji', 'export.options.emoji.hint'),
        check('vp-exp-macrons', 'export.options.macrons.label', 'macrons', null),
        greek ? el('div', { className: 'vp-exp-greek', role: 'group', 'aria-label': T('export.options.greek.label') }, [
          i18nEl('span', 'vp-exp-label', 'export.options.greek.label'),
          radio('vp-exp-greek', 'polytonic', 'export.options.greek.polytonic.label', d.opts.greek === 'polytonic', 'greek'),
          radio('vp-exp-greek', 'monotonic', 'export.options.greek.monotonic.label', d.opts.greek === 'monotonic', 'greek')
        ]) : null,
        el('div', { className: 'vp-row vp-exp-encrow' }, [
          el('div', { className: 'vp-field' }, [
            el('label', { htmlFor: 'vp-exp-enc', 'data-i18n': 'export.options.encoding.label', text: T('export.options.encoding.label') }),
            d.encEl
          ]),
          check('vp-exp-bom', 'export.options.bom.label', 'bom', 'export.options.bom.hint')
        ]),
        isText ? null : check('vp-exp-rebreak', 'export.options.rebreak.label', 'rebreak', null)
      ]),
      el('section', { className: 'vp-exp-section', 'aria-labelledby': 'vp-exp-checks-title' }, [
        i18nEl('h3', null, 'export.checks.title', null, { id: 'vp-exp-checks-title' }),
        d.checksEl
      ]),
      el('section', { className: 'vp-exp-section', 'aria-labelledby': 'vp-exp-preview-title' }, [
        i18nEl('h3', null, 'export.preview.title', null, { id: 'vp-exp-preview-title' }),
        d.previewEl
      ])
    ]);
    return body;
  }

  // ---------------------------------------------------------------- actions
  function setOption(key, value) {
    if (!d) { return false; }
    d.opts[key] = value;
    if (key === 'format') {
      d.path = null;
      d.name = '';
      renderName();
    }
    loadPreview();
    return true;
  }

  function choose() {
    var p = project();
    var name = d.name || fileName(p.name, p.pair, d.opts.format);
    return window.VP_Bridge.call('dialog.saveFile', { suggestedName: name, filters: [d.opts.format] }).then(function (r) {
      if (!d || !r || !r.path || r.cancelled) { return null; }
      d.path = r.path;
      renderName();
      return r.path;
    }, function (err) {
      showError(err);
      return null;
    });
  }

  function targetPath() {
    if (d.path) { return P().resolve(d.path); }
    var name = String(d.name || '').replace(/^\s+|\s+$/g, '');
    if (d.dir && name) { return P().resolve(d.dir + name); }
    return choose();
  }

  function write(path, overwrite) {
    var o = d.opts;
    var params = { path: path, format: o.format, emoji: !!o.emoji, macrons: !!o.macrons, greek: o.greek, encoding: o.encoding, bom: !!o.bom, rebreak: o.rebreak !== false };
    if (overwrite) { params.overwrite = true; }
    return window.VP_Bridge.call('export.write', params);
  }

  function run() {
    if (!d || d.busy) { return P().resolve(null); }
    if (d.checks.fix.length && !d.anyway) { return P().resolve(null); }
    d.busy = true;
    d.exportBtn.disabled = true;
    var finish = function (r) {
      if (d) {
        d.busy = false;
        d.exportBtn.disabled = d.checks.fix.length > 0 && !d.anyway;
      }
      return r;
    };
    return targetPath().then(function (path) {
      if (!path) { return null; }
      return write(path, false).then(null, function (err) {
        if (err && err.code === 'io' && /exists/i.test(String(err.message || err.hint || ''))) {
          return window.VP_Dialog.confirm({ titleKey: 'export.overwrite.title', textKey: 'export.overwrite.text', textVars: { name: path.split(/[\\\/]/).pop() }, confirmKey: 'export.overwrite.confirm' }).then(function (ok) {
            return ok ? write(path, true) : null;
          });
        }
        throw err;
      });
    }).then(function (r) {
      finish();
      if (!r) { return null; }
      var warn = (r.warnings || []).length;
      close();
      window.VP_Toast.show({
        key: warn ? 'export.done.warn.label' : 'export.done.label', vars: { name: String(r.path).split(/[\\\/]/).pop(), n: warn }, kind: 'success',
        actionKey: 'export.reveal.cta', onAction: function () { window.VP_Bridge.call('shell.revealFile', { path: r.path }).then(null, showError); }
      });
      return r;
    }, function (err) {
      finish();
      showError(err);
      return null;
    });
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-exp-action');
    if (a === 'choose') { choose(); } else if (a === 'run') { run(); } else if (a === 'showFast') {
      close();
      if (window.VP_CueList && window.VP_CueList.isMounted()) {
        window.VP_CueList.setFilter('fast');
        window.VP_CueList.focusList();
      }
    } else if (a === 'reviewFix') {
      close();
      if (window.VP_CueList && window.VP_CueList.isMounted()) {
        window.VP_CueList.setFilter('fix');
        if (d === null && window.VP_CueList.firstReview() >= 0) { window.VP_CueList.select(window.VP_CueList.firstReview()); }
        window.VP_CueList.focusList();
      }
    }
  }

  function onChange(e) {
    var t = e.target;
    if (!t || !t.getAttribute) { return; }
    if (t.getAttribute('data-exp-format')) { setOption('format', t.value); } else if (t.getAttribute('data-exp-greek')) { setOption('greek', t.value); } else if (t.getAttribute('data-exp-opt')) {
      setOption(t.getAttribute('data-exp-opt'), !!t.checked);
    } else if (t.getAttribute('data-exp-select')) {
      setOption(t.getAttribute('data-exp-select'), t.value);
    } else if (t.getAttribute('data-exp-anyway')) {
      d.anyway = !!t.checked;
      d.exportBtn.disabled = d.checks.fix.length > 0 && !d.anyway;
    } else if (t.getAttribute('data-exp-name')) {
      d.path = null;
      d.name = String(t.value || '').replace(/^\s+|\s+$/g, '');
      renderName();
    }
  }

  function open() {
    if (d) { return d.handle; }
    if (!window.VP_Store.get('project')) { return null; }
    var ex = settings()['export'] || {};
    var p = project();
    d = {
      opts: { format: originalFormat(), emoji: ex.emoji === true, macrons: ex.macrons === true, greek: 'polytonic', encoding: ex.encoding || 'utf-8', bom: ex.bom === true, rebreak: ex.rebreak !== false },
      path: null, name: '', dir: dirOf(p.sourcePath || p.path), anyway: false, busy: false, previewGen: 0, removers: [], handle: null
    };
    d.checks = checks(storeCues(), window.VP_Store.cueTotal(), cpsLimit());
    var body = build();
    var D = window.VP_Dom;
    D.delegate(body, '[data-exp-action]', 'click', onClick, { owner: OWNER });
    D.on(body, 'change', onChange, { owner: OWNER });
    d.removers.push(window.VP_I18n.onLanguageChanged(function () {
      if (!d) { return; }
      window.VP_I18n.bind(body);
      renderChecks();
    }));
    renderName();
    renderChecks();
    d.handle = window.VP_Dialog.open({
      titleKey: 'export.dialog.title', body: body, className: 'vp-dialog-wide vp-dialog-export',
      actions: [{ labelKey: 'dialog.cancel.cta', value: false, kind: 'secondary' }],
      initialFocus: '#vp-exp-name',
      onClose: function () { teardown(); }
    });
    var actions = D.qs('.vp-dialog-actions', d.handle.el);
    if (actions) { actions.appendChild(d.exportBtn); }
    D.on(d.exportBtn, 'click', function () { run(); }, { owner: OWNER });
    loadPreview();
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
    if (h) { h.close(false); }
    return true;
  }

  function i18nKeys() {
    var keys = ['export.check.cues.label', 'export.check.cuesPartial.label', 'export.check.untranslated.label', 'export.check.fast.label', 'export.check.fix.label', 'export.check.anyway.label',
      'export.done.label', 'export.done.warn.label', 'export.format.hint', 'export.options.greek.label'];
    FORMATS.concat(['txt']).forEach(function (f) { keys.push('export.format.' + f + '.label'); });
    ENCODINGS.forEach(function (e) { keys.push('export.encoding.' + e.replace(/-/g, '') + '.label'); });
    return keys;
  }

  window.VP_Export = {
    open: open,
    close: close,
    isOpen: function () { return d !== null; },
    checks: checks,
    fileName: fileName,
    options: function () { return d ? JSON.parse(JSON.stringify(d.opts)) : null; },
    setOption: setOption,
    setAnyway: function (on) { if (d) { d.anyway = !!on; renderChecks(); } },
    run: run,
    i18nKeys: i18nKeys
  };
}());
