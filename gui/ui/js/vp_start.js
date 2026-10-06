/* vp_start.js - the start screen (DESIGN 13; PREDESIGN 1.1, 4.8): two cards (subtitle file
 * with a drop zone and "Choose file...", typed or pasted text), the language-pair picker
 * (Greek pairs shown disabled, never hidden), "Orbergise a Latin file", recent projects (at
 * most 20, each row a real button), "Try the sample", the status line (dictionaries, model,
 * online check), the recovery banner when project.open finds newer autosaved work, and a
 * red card with the exact fix when the Latin dictionary is missing. Dropping a file anywhere
 * on the window starts the same flow (DOM drop in a browser, dialog.droppedFiles from the
 * shell inside the app).
 * Recent projects: the engine keeps settings.recentProjects (paths, newest first, on open
 * and save); this screen keeps display details in settings.recentInfo {path: {name, pair,
 * kind, cues, translated, needReview, at}} (an extra settings key, unknown keys are kept by
 * the engine). Opening a project normalises the engine's project object and sets VP_Store
 * 'project'; VP_App then routes to the workspace.
 *
 * B8 (real engine): the pair picker is built from engine.hello `pairs` (enabled) and
 * `pairsUnavailable` [{pair, code, message, hint}] (disabled, the reason as tooltip and in a
 * small note under the picker; "not available yet" for the probe's bad_params, else the
 * engine's hint, which names the missing files); before the engine answers, or with an engine
 * without `pairs`, the B6 table applies (Greek "coming later"). The Orbergise option follows
 * the pair la-la the same way. "Try the sample" opens the hello.samples entry of the source
 * language (else <dataDir>/samples/sample.<lang>.srt) and is disabled when the engine lists
 * no sample for it. The status line names the engine (engineKind rules | stub, version).
 *
 * VP_Start.mount(root) / destroy(); openPath(path) -> Promise; openSample(); startText(text);
 * recover() / keepSaved(); pairLabelKey(pair); normalizeProject(raw, extra);
 * remember(project, counts) -> Promise; recentRows(settings); PAIRS; RECENT_MAX;
 * pairInfo(pair, hello?) -> {pair, available, known, code, hint, message}; pairReason(info);
 * sampleFor(lang) -> path | null
 */
(function () {
  'use strict';

  var OWNER = 'screen:start';
  var RECENT_MAX = 20;
  var PAIRS = [
    { code: 'en-la', ready: true }, { code: 'es-la', ready: true }, { code: 'la-en', ready: true }, { code: 'la-es', ready: true },
    { code: 'en-grc', ready: false }, { code: 'es-grc', ready: false }, { code: 'grc-en', ready: false }, { code: 'grc-es', ready: false }
  ];
  var SUBS_EXT = { srt: 1, vtt: 1, ass: 1, ssa: 1 };

  var st = null;

  // ---------------------------------------------------------------- helpers
  function P() { return window.Promise; }

  function camelPair(pair) {
    return String(pair || '').replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
  }

  function pairLabelKey(pair) { return 'start.pair.' + camelPair(pair || 'en-la') + '.label'; }

  function isArray(x) { return Object.prototype.toString.call(x) === '[object Array]'; }

  function hello() {
    var engine = window.VP_Store.get('engine') || {};
    return engine.state === 'ready' ? (engine.hello || null) : null;
  }

  // Availability of one pair from engine.hello (pairs + pairsUnavailable). `known` is false
  // when the engine has not said (not connected yet, or an engine without the list): then the
  // B6 table decides (Latin pairs and Orbergise ready, Greek later).
  function pairInfo(pair, h) {
    h = h === undefined ? hello() : h;
    if (!h || !isArray(h.pairs)) {
      var ready = pair === 'la-la';
      for (var i = 0; i < PAIRS.length; i++) { if (PAIRS[i].code === pair) { ready = PAIRS[i].ready; } }
      return { pair: pair, available: ready, known: false, code: ready ? '' : 'later', hint: '', message: '' };
    }
    if (h.pairs.indexOf(pair) >= 0) { return { pair: pair, available: true, known: true, code: '', hint: '', message: '' }; }
    var list = isArray(h.pairsUnavailable) ? h.pairsUnavailable : [];
    for (var k = 0; k < list.length; k++) {
      if (list[k] && list[k].pair === pair) { return { pair: pair, available: false, known: true, code: list[k].code || 'bad_params', hint: list[k].hint || '', message: list[k].message || '' }; }
    }
    return { pair: pair, available: false, known: true, code: 'bad_params', hint: '', message: '' };
  }

  // Why a pair cannot be used, in the UI language where the engine's code says enough (the
  // probe's bad_params = "not available yet"); otherwise the engine's hint, which names the
  // missing files (dictionary, NLP models) and is engine data like the lexicon notices.
  function pairReason(info) {
    var T = window.VP_I18n;
    if (!info || info.available) { return ''; }
    if (info.code === 'later' || info.code === 'bad_params' || !info.code) { return T.t('start.pair.reason.soon.label'); }
    if (info.hint) { return info.hint; }
    return window.VP_App.errorText(info.code, '').hint;
  }

  function sampleFor(lang) {
    var h = hello();
    if (h && isArray(h.samples)) {
      for (var i = 0; i < h.samples.length; i++) { if (h.samples[i] && h.samples[i].lang === lang && h.samples[i].path) { return h.samples[i].path; } }
      return null;
    }
    var engine = window.VP_Store.get('engine') || {};
    var dir = (engine.hello && engine.hello.dataDir) || '';
    var sep = dir.indexOf('\\') >= 0 ? '\\' : '/';
    return (dir ? dir.replace(/[\\\/]+$/, '') + sep : '') + 'samples' + sep + 'sample.' + lang + '.srt';
  }

  function baseName(path) { return String(path || '').split(/[\\\/]/).pop(); }

  function settings() { return window.VP_Store.get('settings') || {}; }

  function num(v, fallback) { return typeof v === 'number' && isFinite(v) ? v : fallback; }

  // Engine project (engine/cli: {path, autosavePath, manifest, stats, canUndo, canRedo}) or
  // the mock's flat object -> {name, path, kind, pair, cues, translated, needReview, check, fix, ...}.
  function normalizeProject(raw, extra) {
    raw = raw || {};
    var m = raw.manifest || {};
    var s = raw.stats || {};
    var cues = num(raw.cues, num(s.total, 0));
    var check = num(raw.check, num(s.check, 0));
    var fix = num(raw.fix, num(s.fix, 0));
    var out = {
      name: raw.name || m.sourceFileName || baseName(raw.path) || '',
      path: raw.path || null,
      autosavePath: raw.autosavePath || null,
      kind: raw.kind || m.kind || 'subs',
      pair: raw.pair || m.pair || 'en-la',
      cues: cues,
      translated: num(raw.translated, typeof s['new'] === 'number' ? cues - s['new'] : 0),
      reviewed: num(raw.reviewed, num(s.reviewed, 0)),
      needReview: num(raw.needReview, check + fix),
      check: check,
      fix: fix,
      canUndo: !!raw.canUndo,
      canRedo: !!raw.canRedo,
      sourcePath: null
    };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { out[k] = extra[k]; } } }
    return out;
  }

  function recentRows(s) {
    s = s || settings();
    var list = s.recentProjects || [];
    var info = s.recentInfo || {};
    var out = [];
    for (var i = 0; i < list.length && out.length < RECENT_MAX; i++) {
      var e = list[i];
      var path = typeof e === 'string' ? e : (e && e.path);
      if (!path) { continue; }
      var d = (typeof e === 'object' && e) || info[path] || {};
      out.push({ path: path, name: d.name || baseName(path), pair: d.pair || null, kind: d.kind || null, cues: num(d.cues, null), translated: num(d.translated, null), needReview: num(d.needReview, null), at: num(d.at, null) });
    }
    return out;
  }

  function remember(project, counts) {
    if (!project || !project.path || !window.VP_App || typeof window.VP_App.saveSettings !== 'function') { return P().resolve(false); }
    var s = settings();
    var paths = [project.path].concat((s.recentProjects || []).filter(function (x) { return typeof x === 'string' && x !== project.path; })).slice(0, RECENT_MAX);
    var old = s.recentInfo || {};
    var info = {};
    for (var i = 0; i < paths.length; i++) { if (old[paths[i]]) { info[paths[i]] = old[paths[i]]; } }
    var c = counts || {};
    info[project.path] = {
      name: project.name || baseName(project.path), pair: project.pair, kind: project.kind,
      cues: num(c.all, project.cues), translated: num(c.translated, project.translated), needReview: num(c.review, project.needReview),
      at: new Date().getTime()
    };
    return window.VP_App.saveSettings({ recentInfo: info }).then(function () { return true; }, function () { return false; });
  }

  function whenText(at) {
    var T = window.VP_I18n;
    if (typeof at !== 'number') { return ''; }
    var diff = Math.max(0, new Date().getTime() - at);
    var min = Math.floor(diff / 60000);
    if (min < 1) { return T.t('start.recent.when.now'); }
    if (min < 60) { return T.t('start.recent.when.minutes', { n: min }); }
    var h = Math.floor(min / 60);
    if (h < 24) { return T.t('start.recent.when.hours', { n: h }); }
    var days = Math.floor(h / 24);
    if (days < 7) { return T.t('start.recent.when.days', { n: days }); }
    var d = new Date(at);
    return T.t('start.recent.when.date', { d: d.getDate(), m: d.getMonth() + 1, y: d.getFullYear() });
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function svgIcon(d) {
    var D = window.VP_Dom;
    return D.el('svg', { className: 'vp-icon', viewBox: '0 0 16 16', 'aria-hidden': 'true', focusable: 'false' }, [D.el('path', { d: d })]);
  }

  // ---------------------------------------------------------------- opening
  function setBusy(on) {
    if (!st) { return; }
    st.busy = on;
    st.root.setAttribute('aria-busy', on ? 'true' : 'false');
    window.VP_Dom.qsa('[data-start-action]', st.root).forEach(function (b) { b.disabled = on; });
    if (!on) { renderSample(); }
  }

  function currentPair() {
    if (!st) { return settings().defaultPair || 'en-la'; }
    return st.orberg.checked && !st.orberg.disabled ? 'la-la' : (st.pair.value || 'en-la');
  }

  function enter(result, extra) {
    var proj = normalizeProject(result && result.project, extra);
    window.VP_Store.clearCues();
    window.VP_History.clear();
    window.VP_History.syncFromEngine({ canUndo: proj.canUndo, canRedo: proj.canRedo });
    window.VP_Store.set('selection', null);
    window.VP_Store.set('saveState', proj.path ? { kind: 'saved', at: new Date().getTime() } : { kind: 'unsaved', at: 0 });
    if (proj.path) { remember(proj, null); }
    window.VP_Store.set('project', proj);
    return proj;
  }

  function run(promise, onDone) {
    setBusy(true);
    return promise.then(function (r) {
      setBusy(false);
      return onDone(r);
    }, function (err) {
      setBusy(false);
      showError(err);
      return null;
    });
  }

  function extOf(path) {
    var m = /\.([A-Za-z0-9]+)$/.exec(String(path || ''));
    return m ? m[1].toLowerCase() : '';
  }

  function openPath(path) {
    var B = window.VP_Bridge;
    var ext = extOf(path);
    if (!path) { return P().resolve(null); }
    if (ext === 'vpoeta') {
      return run(B.call('project.open', { path: path }), function (r) {
        if (r.recoverable && st) {
          showRecovery(path, r);
          return null;
        }
        return enter(r);
      });
    }
    if (SUBS_EXT[ext] || ext === 'txt') {
      return run(B.call('project.new', { kind: ext === 'txt' ? 'text' : 'subs', pair: currentPair(), sourcePath: path }), function (r) {
        return enter(r, { sourcePath: path });
      });
    }
    showError({ code: 'unsupported_format' });
    return P().resolve(null);
  }

  function chooseFile() {
    return run(window.VP_Bridge.call('dialog.openFile', { kind: 'subtitles', filters: ['srt', 'vtt', 'ass', 'ssa', 'txt', 'vpoeta'] }), function (r) {
      var path = r && (r.path || (r.paths && r.paths[0]));
      return path ? openPath(path) : null;
    });
  }

  function samplePath(pair) { return sampleFor(String(pair || 'en-la').split('-')[0]); }

  function openSample() {
    var pair = currentPair();
    var path = samplePath(pair);
    if (!path) {
      window.VP_Toast.show({ key: 'start.sample.none.label', vars: { lang: langName(String(pair).split('-')[0]) }, kind: 'error' });
      return P().resolve(null);
    }
    return run(window.VP_Bridge.call('project.new', { kind: 'subs', pair: pair, sourcePath: path }), function (r) {
      return enter(r, { sourcePath: path, sample: true });
    });
  }

  function startText(text) {
    var value = String(text === undefined && st ? st.text.value : text || '');
    if (!value.replace(/\s+/g, '')) {
      if (st) {
        st.textError.hidden = false;
        st.text.setAttribute('aria-invalid', 'true');
        st.text.focus();
      }
      return P().resolve(null);
    }
    if (st) {
      st.textError.hidden = true;
      st.text.removeAttribute('aria-invalid');
    }
    return run(window.VP_Bridge.call('project.new', { kind: 'text', pair: currentPair(), text: value }), function (r) { return enter(r); });
  }

  // ---------------------------------------------------------------- recovery banner
  function showRecovery(path, result) {
    st.recovery = { path: path, result: result };
    var rec = result.recoverable || {};
    var at = rec.at ? Date.parse(rec.at) : NaN;
    st.recoveryText.setAttribute('data-i18n-vars', JSON.stringify({ name: baseName(path), when: isNaN(at) ? '' : whenText(at) }));
    window.VP_I18n.bind(st.recoveryEl);
    st.recoveryEl.hidden = false;
    var b = window.VP_Dom.qs('[data-start-action="recover"]', st.recoveryEl);
    if (b) { b.focus(); }
  }

  function hideRecovery() {
    if (!st) { return; }
    st.recoveryEl.hidden = true;
    st.recovery = null;
  }

  function recover() {
    if (!st || !st.recovery) { return P().resolve(null); }
    var rec = st.recovery;
    return run(window.VP_Bridge.call('project.recover', { path: rec.path }), function (r) {
      hideRecovery();
      return enter(r && r.project ? r : rec.result);
    });
  }

  function keepSaved() {
    if (!st || !st.recovery) { return null; }
    var rec = st.recovery;
    hideRecovery();
    return enter(rec.result);
  }

  // ---------------------------------------------------------------- rendering
  function renderRecent() {
    if (!st) { return; }
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var rows = recentRows();
    D.clear(st.recentList);
    st.recentEmpty.hidden = rows.length > 0;
    st.rows = rows;
    for (var i = 0; i < rows.length; i++) {
      var r = rows[i];
      var parts = [D.el('span', { className: 'vp-recent-name', text: r.name })];
      if (r.pair) { parts.push(D.el('span', { className: 'vp-recent-pair', text: T.t(pairLabelKey(r.pair)) })); }
      if (r.cues !== null) { parts.push(D.el('span', { className: 'vp-recent-progress', text: T.t(r.kind === 'text' ? 'start.recent.paragraphs' : 'start.recent.progress', { done: r.translated || 0, total: r.cues, n: r.cues }) })); }
      if (r.needReview) {
        parts.push(D.el('span', { className: 'vp-recent-review', text: T.t('start.recent.review', { n: r.needReview }) }));
      } else if (r.cues && r.translated === r.cues) {
        parts.push(D.el('span', { className: 'vp-recent-done', text: T.t('start.recent.done.label') }));
      }
      parts.push(D.el('span', { className: 'vp-recent-when', text: r.at !== null ? whenText(r.at) : '' }));
      st.recentList.appendChild(D.el('li', null, [D.el('button', { type: 'button', className: 'vp-recent-row', title: r.path, 'data-recent': String(i) }, parts)]));
    }
  }

  function lexiconProblem(engine) {
    if (!engine) { return null; }
    if (engine.state === 'failed' && /^lexicon_/.test(engine.code || '')) { return { code: engine.code, path: '' }; }
    if (engine.state !== 'ready') { return null; }
    var list = (engine.hello && engine.hello.lexicons) || [];
    for (var i = 0; i < list.length; i++) {
      if (list[i].lang === 'la') {
        if (list[i].available === false) { return { code: (list[i].error && list[i].error.code) || 'lexicon_missing', path: list[i].path || '' }; }
        return null;
      }
    }
    return { code: 'lexicon_missing', path: '' };
  }

  function langName(code) {
    var key = 'app.lexicon.lang.' + code;
    return window.VP_I18n.has(key) ? window.VP_I18n.t(key) : code;
  }

  function renderStatus() {
    if (!st) { return; }
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var engine = window.VP_Store.get('engine') || { state: 'connecting' };
    var s = settings();
    var parts = [];
    if (engine.state === 'ready') {
      var hello = engine.hello || {};
      if (engine.kind === 'mock') {
        parts.push(D.el('span', { 'data-i18n': 'app.engine.mock.label', 'data-i18n-vars': { version: hello.version || '?' } }));
      } else if (hello.engineKind === 'rules' || hello.engineKind === 'stub') {
        parts.push(D.el('span', { className: 'vp-start-engine vp-start-engine-' + hello.engineKind, 'data-i18n': 'start.status.engine.' + hello.engineKind + '.label', 'data-i18n-vars': { version: hello.version || hello.engine || '?' } }));
      }
      var any = false;
      (hello.lexicons || []).forEach(function (lx) {
        if (lx.available === false) { return; }
        any = true;
        var name = T.has('app.lexicon.lang.' + lx.lang) ? T.t('app.lexicon.lang.' + lx.lang) : lx.lang;
        parts.push(D.el('span', { 'data-i18n': 'app.lexicon', 'data-i18n-vars': { lang: name, version: lx.version || '?', n: lx.lemmas || 0 } }));
      });
      if (!any) { parts.push(D.el('span', { 'data-i18n': 'app.lexicon.none.label' })); }
      var m = hello.model || {};
      parts.push(D.el('span', { 'data-i18n': m.available ? 'app.model.ready.label' : (m.reason === 'not_built' ? 'start.status.model.notBuilt.label' : 'app.model.missing.label') }));
      parts.push(D.el('span', { 'data-i18n': s.engines && s.engines.online ? 'start.status.online.on.label' : 'start.status.online.off.label' }));
    } else if (engine.state === 'failed') {
      parts.push(D.el('span', { 'data-i18n': 'app.engine.failed.label' }));
      parts.push(D.el('span', { text: window.VP_App.errorText(engine.code, engine.hint).title }));
    } else if (engine.state === 'none') {
      parts.push(D.el('span', { 'data-i18n': 'app.engine.none.label' }));
    } else {
      parts.push(D.el('span', { 'data-i18n': 'app.engine.connecting.label' }));
    }
    D.clear(st.status);
    D.append(st.status, parts);
    st.status.setAttribute('data-state', engine.state);
    T.bind(st.status);

    var problem = lexiconProblem(engine);
    st.lexCard.hidden = !problem;
    if (problem) {
      var e = window.VP_App.errorText(problem.code, '');
      st.lexTitle.textContent = e.title;
      st.lexHint.textContent = e.hint;
      st.lexPath.hidden = !problem.path;
      if (problem.path) {
        st.lexPath.setAttribute('data-i18n-vars', JSON.stringify({ path: problem.path }));
        T.bind(st.lexPath);
      }
    }
  }

  // The picker's options, the Orbergise option and the note, from engine.hello (pairInfo).
  function renderPairs() {
    if (!st) { return; }
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var h = hello();
    var keep = st.pair.value;
    var infos = PAIRS.map(function (x) { return pairInfo(x.code, h); });
    D.clear(st.pair);
    infos.forEach(function (info) {
      var name = T.t(pairLabelKey(info.pair));
      var label = info.available ? name : T.t(info.code === 'later' ? 'start.pair.later.label' : 'start.pair.unavailable.label', { pair: name });
      st.pair.appendChild(D.el('option', { value: info.pair, disabled: !info.available, title: info.available ? null : pairReason(info), text: label }));
    });
    var want = !st.pairTouched && settings().defaultPair ? settings().defaultPair : keep;
    var pick = null;
    infos.forEach(function (info) { if (info.available && info.pair === want) { pick = want; } });
    if (!pick && infos.some(function (i) { return i.available && i.pair === keep; })) { pick = keep; }
    if (!pick) { infos.forEach(function (info) { if (!pick && info.available) { pick = info.pair; } }); }
    st.pair.value = pick || 'en-la';
    var orb = pairInfo('la-la', h);
    st.orberg.disabled = !orb.available;
    if (!orb.available) { st.orberg.checked = false; }
    st.orbergWrap.className = 'vp-start-check' + (orb.available ? '' : ' vp-start-check-off');
    st.orbergWrap.setAttribute('title', orb.available ? '' : pairReason(orb));
    // The note: one line per reason, naming its pairs (only when the engine told us).
    D.clear(st.pairNote);
    var groups = [];
    var byReason = {};
    infos.concat([orb]).forEach(function (info) {
      if (info.available || !info.known) { return; }
      var why = pairReason(info);
      if (byReason[why] === undefined) {
        byReason[why] = groups.length;
        groups.push({ why: why, names: [] });
      }
      groups[byReason[why]].names.push(T.t(pairLabelKey(info.pair)));
    });
    st.pairNote.hidden = !groups.length;
    groups.forEach(function (g) {
      st.pairNote.appendChild(D.el('li', { className: 'vp-hint', text: T.t('start.pair.note.item', { pairs: g.names.join(', '), why: g.why }) }));
    });
    renderPair();
    renderSample();
  }

  function renderPair() {
    if (!st) { return; }
    st.pair.disabled = st.orberg.checked;
  }

  function renderSample() {
    if (!st || !st.sampleBtn || st.busy) { return; }
    var lang = currentPair().split('-')[0];
    var path = samplePath(currentPair());
    st.sampleBtn.disabled = !path;
    st.sampleBtn.setAttribute('title', path ? '' : window.VP_I18n.t('start.sample.none.label', { lang: langName(lang) }));
  }

  function langButtons() {
    if (!st) { return; }
    var cur = window.VP_I18n.lang();
    window.VP_Dom.qsa('[data-lang]', st.root).forEach(function (b) { b.setAttribute('aria-pressed', b.getAttribute('data-lang') === cur ? 'true' : 'false'); });
  }

  // ---------------------------------------------------------------- events
  function onClick(e, btn) {
    var a = btn.getAttribute('data-start-action');
    var lang = btn.getAttribute('data-lang');
    var recent = btn.getAttribute('data-recent');
    if (lang) {
      window.VP_App.setLang(lang);
      langButtons();
      renderRecent();
    } else if (recent !== null) {
      var row = st.rows[Number(recent)];
      if (row) { openPath(row.path); }
    } else if (a === 'choose') {
      chooseFile();
    } else if (a === 'text') {
      startText();
    } else if (a === 'sample') {
      openSample();
    } else if (a === 'recover') {
      recover();
    } else if (a === 'keep') {
      keepSaved();
    } else if (a === 'settings') {
      if (window.VP_Settings) { window.VP_Settings.open(); } else { window.VP_Dialog.open({ titleKey: 'workspace.settings.title', textKey: 'workspace.settings.soon.text', actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }] }); }
    } else if (a === 'help') {
      if (typeof window.VP_App.openHelp === 'function') { window.VP_App.openHelp(); } else { window.VP_App.openShortcuts(); }
    }
  }

  function hasFiles(e) {
    var dt = e.dataTransfer;
    if (!dt) { return false; }
    var types = dt.types || [];
    for (var i = 0; i < types.length; i++) { if (types[i] === 'Files') { return true; } }
    return !!(dt.files && dt.files.length);
  }

  function dragState(on) {
    if (!st) { return; }
    st.overlay.hidden = !on;
    if (on) { st.root.classList.add('vp-dragging'); } else { st.root.classList.remove('vp-dragging'); }
  }

  function onDragEnter(e) {
    if (!hasFiles(e)) { return; }
    e.preventDefault();
    st.dragDepth++;
    dragState(true);
  }

  function onDragOver(e) {
    if (!hasFiles(e)) { return; }
    e.preventDefault();
    if (e.dataTransfer) { e.dataTransfer.dropEffect = 'copy'; }
  }

  function onDragLeave() {
    st.dragDepth = Math.max(0, st.dragDepth - 1);
    if (!st.dragDepth) { dragState(false); }
  }

  function onDrop(e) {
    if (!hasFiles(e)) { return; }
    e.preventDefault();
    st.dragDepth = 0;
    dragState(false);
    if (window.VP_Bridge.isNative()) { return; }
    var f = e.dataTransfer.files && e.dataTransfer.files[0];
    if (f) { openPath(f.path || f.name); }
  }

  function build(root) {
    var D = window.VP_Dom;
    st.pair = D.el('select', { id: 'vp-start-pair', className: 'vp-input', 'aria-describedby': 'vp-start-pair-note' });
    st.pairNote = D.el('ul', { id: 'vp-start-pair-note', className: 'vp-start-pair-note', hidden: true });
    st.orberg = D.el('input', { id: 'vp-start-orberg', type: 'checkbox', 'aria-describedby': 'vp-start-pair-note' });
    st.orbergWrap = D.el('span', { className: 'vp-start-check' }, [st.orberg, D.el('label', { htmlFor: 'vp-start-orberg', 'data-i18n': 'start.orberg.label' })]);
    st.sampleBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-start-action': 'sample', 'data-i18n': 'start.sample.cta' });
    st.text = D.el('textarea', { id: 'vp-start-text', className: 'vp-input vp-start-text', rows: '4', 'aria-describedby': 'vp-start-text-hint', 'data-i18n-placeholder': 'start.text.placeholder' });
    st.textError = D.el('p', { id: 'vp-start-text-error', className: 'vp-field-error', role: 'alert', hidden: true, 'data-i18n': 'start.text.empty' });
    st.recentList = D.el('ul', { className: 'vp-recent-list', 'aria-labelledby': 'vp-start-recent-title' });
    st.recentEmpty = D.el('p', { className: 'vp-hint vp-recent-empty', 'data-i18n': 'start.recent.empty' });
    st.status = D.el('p', { id: 'vp-start-status', className: 'vp-status-line vp-start-status', 'aria-live': 'polite' });
    st.lexTitle = D.el('h2');
    st.lexHint = D.el('p');
    st.lexPath = D.el('p', { className: 'vp-mono', 'data-i18n': 'start.lexicon.path.label' });
    st.lexCard = D.el('section', { className: 'vp-card vp-card-error', role: 'alert', hidden: true }, [st.lexTitle, st.lexHint, st.lexPath]);
    st.recoveryText = D.el('p', { 'data-i18n': 'start.recovery.text' });
    st.recoveryEl = D.el('section', { className: 'vp-banner', role: 'region', 'aria-labelledby': 'vp-start-recovery-title', hidden: true }, [
      D.el('div', { className: 'vp-banner-text' }, [
        D.el('h2', { id: 'vp-start-recovery-title', 'data-i18n': 'start.recovery.title' }),
        st.recoveryText
      ]),
      D.el('div', { className: 'vp-row' }, [
        D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', 'data-start-action': 'recover', 'data-i18n': 'start.recovery.recover.cta' }),
        D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-start-action': 'keep', 'data-i18n': 'start.recovery.keep.cta' })
      ])
    ]);
    st.overlay = D.el('div', { className: 'vp-drop-overlay', hidden: true, 'aria-hidden': 'true' }, [D.el('p', { 'data-i18n': 'start.drop.overlay.label' })]);
    st.root = D.el('section', { className: 'vp-start', 'aria-labelledby': 'vp-start-title', 'aria-busy': 'false' }, [
      D.el('div', { className: 'vp-start-top' }, [
        D.el('h1', { id: 'vp-start-title', tabIndex: -1, 'data-i18n': 'start.title' }),
        D.el('div', { className: 'vp-switches' }, [
          D.el('div', { className: 'vp-segmented', role: 'group', 'data-i18n-aria': 'app.lang.switch.aria' }, [
            D.el('button', { type: 'button', className: 'vp-seg', 'data-lang': 'en-US', lang: 'en', 'aria-pressed': 'false', 'data-i18n': 'app.lang.en.label' }),
            D.el('button', { type: 'button', className: 'vp-seg', 'data-lang': 'es-MX', lang: 'es', 'aria-pressed': 'false', 'data-i18n': 'app.lang.es.label' })
          ]),
          D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon', 'data-start-action': 'settings', 'data-i18n-aria': 'workspace.settings.aria', 'data-i18n-title': 'workspace.settings.aria' }, [
            svgIcon('M8 5.5a2.5 2.5 0 1 0 0 5a2.5 2.5 0 1 0 0-5zM8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2M3.4 3.4l1.4 1.4M11.2 11.2l1.4 1.4M3.4 12.6l1.4-1.4M11.2 4.8l1.4-1.4')
          ]),
          D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon', 'data-start-action': 'help', 'data-i18n-aria': 'workspace.help.aria', 'data-i18n-title': 'workspace.help.aria' }, [
            svgIcon('M8 14.5a6.5 6.5 0 1 0 0-13a6.5 6.5 0 1 0 0 13zM6 6.2a2 2 0 1 1 2.6 1.9c-.4.2-.6.5-.6.9v.8M8 11.5v.1')
          ])
        ])
      ]),
      st.lexCard,
      st.recoveryEl,
      D.el('div', { className: 'vp-start-cards' }, [
        D.el('section', { className: 'vp-card vp-start-card vp-drop-zone', 'aria-labelledby': 'vp-start-file-title' }, [
          D.el('div', { className: 'vp-start-card-head' }, [
            svgIcon('M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M5.5 8.5h5M5.5 11h5'),
            D.el('h2', { id: 'vp-start-file-title', 'data-i18n': 'start.file.title' })
          ]),
          D.el('p', { className: 'vp-hint', 'data-i18n': 'start.file.hint' }),
          D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', 'data-start-action': 'choose', 'aria-keyshortcuts': 'Control+O', 'data-i18n': 'start.file.cta' })
        ]),
        D.el('section', { className: 'vp-card vp-start-card', 'aria-labelledby': 'vp-start-text-title' }, [
          D.el('div', { className: 'vp-start-card-head' }, [
            svgIcon('M2.5 13.5l1-3.5l7.5-7.5l2.5 2.5l-7.5 7.5zM9.5 4l2.5 2.5'),
            D.el('h2', { id: 'vp-start-text-title' }, [D.el('label', { htmlFor: 'vp-start-text', 'data-i18n': 'start.text.title' })])
          ]),
          D.el('p', { id: 'vp-start-text-hint', className: 'vp-hint', 'data-i18n': 'start.text.hint' }),
          st.text,
          st.textError,
          D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-start-action': 'text', 'data-i18n': 'start.text.cta' })
        ])
      ]),
      D.el('div', { className: 'vp-start-pairs' }, [
        D.el('label', { htmlFor: 'vp-start-pair', 'data-i18n': 'start.pair.label' }),
        st.pair,
        st.orbergWrap
      ]),
      st.pairNote,
      D.el('div', { className: 'vp-start-recent-head' }, [
        D.el('h2', { id: 'vp-start-recent-title', 'data-i18n': 'start.recent.title' }),
        st.sampleBtn
      ]),
      st.recentList,
      st.recentEmpty,
      st.status,
      st.overlay
    ]);
    root.appendChild(st.root);
  }

  function mount(root) {
    if (st) { destroy(); }
    st = { busy: false, recovery: null, rows: [], dragDepth: 0, pairTouched: false, removers: [] };
    build(root);
    var D = window.VP_Dom;
    D.delegate(st.root, 'button', 'click', onClick, { owner: OWNER });
    D.on(st.pair, 'change', function () {
      st.pairTouched = true;
      renderSample();
      if (window.VP_App && typeof window.VP_App.saveSettings === 'function') { window.VP_App.saveSettings({ defaultPair: st.pair.value }); }
    }, { owner: OWNER });
    D.on(st.orberg, 'change', function () {
      renderPair();
      renderSample();
    }, { owner: OWNER });
    D.on(st.text, 'input', function () {
      if (!st.textError.hidden) {
        st.textError.hidden = true;
        st.text.removeAttribute('aria-invalid');
      }
    }, { owner: OWNER });
    var html = document.documentElement;
    D.on(html, 'dragenter', onDragEnter, { owner: OWNER });
    D.on(html, 'dragover', onDragOver, { owner: OWNER });
    D.on(html, 'dragleave', onDragLeave, { owner: OWNER });
    D.on(html, 'drop', onDrop, { owner: OWNER });
    st.removers.push(window.VP_Bridge.on('dialog.droppedFiles', function (e) {
      var path = e && e.paths && e.paths[0];
      if (path && st && !st.busy) { openPath(path); }
    }));
    st.removers.push(window.VP_Store.subscribe('engine', function () {
      renderStatus();
      renderPairs();
    }));
    st.removers.push(window.VP_Store.subscribe('settings', function () {
      renderStatus();
      renderRecent();
      if (!st.pairTouched) { renderPairs(); }
    }));
    st.removers.push(window.VP_I18n.onLanguageChanged(function () {
      langButtons();
      renderRecent();
      renderStatus();
      renderPairs();
    }));
    st.removers.push(window.VP_Keys.handle('open', function () { chooseFile(); }, OWNER));
    st.removers.push(window.VP_Keys.handle('new', function () { st.text.focus(); }, OWNER));
    renderPairs();
    renderRecent();
    renderStatus();
    langButtons();
    window.VP_I18n.bind(st.root);
  }

  function destroy() {
    if (!st) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (st.removers.length) { st.removers.pop()(); }
    if (st.root && st.root.parentNode) { st.root.parentNode.removeChild(st.root); }
    st = null;
  }

  function i18nKeys() {
    var keys = ['start.recent.when.now', 'start.recent.when.minutes', 'start.recent.when.hours', 'start.recent.when.days', 'start.recent.when.date',
      'start.recent.progress', 'start.recent.paragraphs', 'start.recent.review', 'start.pair.later.label', 'start.pair.unavailable.label', 'start.pair.reason.soon.label',
      'start.pair.note.item', 'start.sample.none.label', 'start.status.engine.rules.label', 'start.status.engine.stub.label', 'start.status.model.notBuilt.label'];
    for (var i = 0; i < PAIRS.length; i++) { keys.push(pairLabelKey(PAIRS[i].code)); }
    keys.push(pairLabelKey('la-la'));
    return keys;
  }

  window.VP_Start = {
    PAIRS: PAIRS,
    RECENT_MAX: RECENT_MAX,
    mount: mount,
    destroy: destroy,
    isMounted: function () { return st !== null; },
    openPath: openPath,
    chooseFile: chooseFile,
    openSample: openSample,
    samplePath: samplePath,
    startText: startText,
    recover: recover,
    keepSaved: keepSaved,
    recoveryShown: function () { return !!(st && st.recovery); },
    pairLabelKey: pairLabelKey,
    pairInfo: pairInfo,
    pairReason: pairReason,
    sampleFor: sampleFor,
    normalizeProject: normalizeProject,
    remember: remember,
    recentRows: recentRows,
    i18nKeys: i18nKeys
  };
}());
