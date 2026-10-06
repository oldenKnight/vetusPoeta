/* vp_workspace.js - the project workspace (DESIGN 13; PREDESIGN 1.2, 4.1, 4.3, 4.4, 4.8).
 *
 * Layout: top bar (wordmark that closes the project, name, save state, pair, mode tabs
 * Translate | Orbergise, Undo/Redo, drawer toggle, gear, ?), three columns (cue list 320 px,
 * centre >= 560 px with the translate strip and VP_Panes, right panel host 360 px that the
 * right-panel tabs fill; below 1180 px it becomes a drawer), status bar (autosave text in an
 * aria-live region, counts, engines, network indicator).
 * The workspace owns the shortcut handlers of PREDESIGN 4.3 while it is mounted and the
 * engine commands that change cues (VP_Workspace.cmd, also used by VP_CueList and
 * VP_Panes): each updates VP_Store at once, mirrors the step in VP_History with before/after
 * snapshots, and lets the engine stay authoritative (undo/redo reconcile with its
 * changedIndices). Translation progress arrives through VP_Bridge, already coalesced to at
 * most 10 UI updates per second.
 *
 * VP_Workspace.mount(root, params) / destroy(); panelHost(); mode() / setMode('translate' |
 * 'orberg'); toggleDrawer(open?) / drawerOpen(); save() / close() -> Promise; isMounted()
 * VP_Workspace.cmd: review(indices, reviewed, {labelKey, toastKey}), edit(index, text,
 *   remember), choose(index, alternative), undo(), redo(), translate(indices?),
 *   orbergise(opts), cancel(), refetch(indices) -> Promise; markStale() -> count (UI-side:
 *   translated cues get the grey dot after an engine setting changed; edited and reviewed
 *   cues are never touched)
 * Right panel (B7): a tab strip Word | Engines | Names | Corrections | Words over
 * panelHost(); each tab is a module with mount(el)/destroy(); the last tab is remembered in
 * the settings key panelTab (UI-only). VP_Workspace.setTab(id) / tab(); PANEL_TABS. The gear
 * opens VP_Settings, ? opens VP_App.openHelp, the Export button and Ctrl+Shift+E open
 * VP_Export, the Orbergise mode tab mounts VP_Orberg in its tab panel.
 * Store keys it writes: selection {index}, job, saveState {kind, at}, inspect (VP_Panes),
 * cueCounts (VP_CueList).
 * B8: engine warnings (VP_Store 'warnings', one entry {engine, code, message, hint, jobId}
 * per engine): the translate.start result's `warnings` codes replace the list, then the
 * job's translate.warning events (start, or a load/network failure mid-job) and any
 * translate.done `warnings` refine it. A new warning shows a toast once per job and engine
 * with "Show engines"; while the list is not empty the status bar carries a chip per engine
 * ("Model unavailable: not installed") that opens the Engines tab. cmd.edit publishes the
 * engine's correctionAdded object {id, key, target, scope, count} as VP_Store
 * 'correctionAdded' (the Corrections tab reloads). Ctrl+I toggles the interlinear lines of a
 * Latin/Greek source (VP_Panes.toggleInterlinear). The root carries data-pair and, for Greek
 * pairs, the class pair-grc (Aegean accent, PREDESIGN 2.3), also on the app element.
 */
(function () {
  'use strict';

  var OWNER = 'workspace';
  var PAGE = 200;
  var KEY_ACTIONS = ['nextCue', 'prevCue', 'nextReview', 'prevReview', 'accept', 'acceptNext', 'edit', 'cancelEdit', 'acceptEdit',
    'alt1', 'alt2', 'alt3', 'search', 'macrons', 'emoji', 'interlinear', 'undo', 'redo', 'save', 'export', 'settings'];
  var WARN_ENGINES = ['model', 'online'];
  var INTERACTIVE = 'button, a, input, select, textarea, [role="button"], [role="tab"], [role="option"]';
  var PANEL_TABS = [
    { id: 'word', module: 'VP_Inspector', key: 'workspace.tab.word.label' },
    { id: 'engines', module: 'VP_Engines', key: 'workspace.tab.engines.label' },
    { id: 'names', module: 'VP_Names', key: 'workspace.tab.names.label' },
    { id: 'corrections', module: 'VP_Corrections', key: 'workspace.tab.corrections.label' },
    { id: 'words', module: 'VP_Words', key: 'workspace.tab.words.label' }
  ];

  var w = null;

  // ---------------------------------------------------------------- small helpers
  function P() { return window.Promise; }

  function copy(obj, patch) {
    var o = {};
    var k;
    for (k in obj) { if (Object.prototype.hasOwnProperty.call(obj, k)) { o[k] = obj[k]; } }
    for (k in patch) { if (Object.prototype.hasOwnProperty.call(patch, k) && k !== 'index') { o[k] = patch[k]; } }
    return o;
  }

  function snap(c, fields) {
    var o = { index: c.index };
    for (var i = 0; i < fields.length; i++) {
      var v = c[fields[i]];
      o[fields[i]] = Object.prototype.toString.call(v) === '[object Array]' ? v.slice() : v;
    }
    return o;
  }

  function applySnaps(list) {
    var out = [];
    for (var i = 0; i < (list || []).length; i++) {
      var c = window.VP_Store.getCue(list[i].index);
      if (c) { out.push(copy(c, list[i])); }
    }
    if (out.length) { window.VP_Store.putCues(out); }
    return out.length;
  }

  function sameSet(a, b) {
    if (!a || !b || a.length !== b.length) { return false; }
    var x = a.slice().sort(function (p, q) { return p - q; });
    var y = b.slice().sort(function (p, q) { return p - q; });
    for (var i = 0; i < x.length; i++) { if (x[i] !== y[i]) { return false; } }
    return true;
  }

  function settings() { return window.VP_Store.get('settings') || {}; }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  function clock(at) {
    var d = new Date(at);
    return pad2(d.getHours()) + ':' + pad2(d.getMinutes());
  }

  function setText(node, key, vars) {
    node.setAttribute('data-i18n', key);
    if (vars) { node.setAttribute('data-i18n-vars', JSON.stringify(vars)); } else { node.removeAttribute('data-i18n-vars'); }
    window.VP_I18n.bind(node);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  // A change is "pending" until the engine reports project.autosaved (or a save).
  function markChanged() {
    var cur = window.VP_Store.get('saveState') || {};
    if (cur.kind !== 'pending') { window.VP_Store.set('saveState', { kind: 'pending', at: cur.at || 0 }); }
  }

  function onAutosaved(e) {
    var at = e && e.at ? Date.parse(e.at) : NaN;
    window.VP_Store.set('saveState', { kind: 'autosaved', at: isNaN(at) ? new Date().getTime() : at });
  }

  function svgIcon(d) {
    var D = window.VP_Dom;
    return D.el('svg', { className: 'vp-icon', viewBox: '0 0 16 16', 'aria-hidden': 'true', focusable: 'false' }, [D.el('path', { d: d })]);
  }

  function useIcon(id) {
    var D = window.VP_Dom;
    return D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' }, [D.el('use', { href: id })]);
  }

  // ---------------------------------------------------------------- commands
  function refetch(indices) {
    var pages = {};
    var list = [];
    for (var i = 0; i < (indices || []).length; i++) {
      var p = Math.floor(indices[i] / PAGE);
      if (!pages[p]) {
        pages[p] = true;
        list.push(p);
      }
    }
    var chain = P().resolve(0);
    list.forEach(function (page) {
      chain = chain.then(function () {
        return window.VP_Bridge.call('cue.page', { from: page * PAGE, count: PAGE }).then(function (r) {
          window.VP_Store.putCues(r.cues || []);
        });
      });
    });
    return chain.then(function () { return list.length; });
  }

  function review(indices, reviewed, opts) {
    opts = opts || {};
    var before = [];
    var after = [];
    for (var i = 0; i < indices.length; i++) {
      var c = window.VP_Store.getCue(indices[i]);
      if (!c || c.state === 'new') { continue; }
      var next = reviewed ? 'reviewed' : (c.state === 'reviewed' ? 'translated' : c.state);
      if (next === c.state) { continue; }
      before.push({ index: c.index, state: c.state });
      after.push({ index: c.index, state: next });
    }
    if (!after.length) { return P().resolve(0); }
    applySnaps(after);
    var idx = after.map(function (x) { return x.index; });
    return window.VP_Bridge.call('cue.review', { indices: idx, reviewed: !!reviewed }).then(function (r) {
      window.VP_History.push({ kind: 'review', labelKey: opts.labelKey || 'workspace.history.accept.label', indices: idx, before: before, after: after });
      markChanged();
      if (opts.toastKey) {
        window.VP_Toast.undoable(opts.toastKey, { n: r.count }, function () { undo(); });
      }
      return r.count;
    }, function (err) {
      applySnaps(before);
      throw err;
    });
  }

  // The engine answers {id, key, target, scope, count} (an older engine: the id alone).
  function correctionOf(x) {
    if (!x) { return null; }
    if (typeof x === 'object') { return { id: x.id, key: x.key || '', target: x.target || '', scope: x.scope || '', count: typeof x.count === 'number' ? x.count : 0 }; }
    return { id: String(x), key: '', target: '', scope: '', count: 0 };
  }

  function edit(index, text, remember) {
    var c = window.VP_Store.getCue(index);
    if (!c) { return P().reject(new Error('no cue ' + index)); }
    var rememberOnly = !!remember && text === c.target && c.state === 'edited';
    var before = snap(c, ['target', 'state', 'lines']);
    var params = { index: index, text: text };
    if (remember) { params.remember = remember; }
    return window.VP_Bridge.call('cue.set', params).then(function (r) {
      window.VP_Store.putCues([r.cue]);
      if (!rememberOnly) {
        window.VP_History.push({ kind: 'edit', labelKey: 'workspace.history.edit.label', indices: [index], before: [before], after: [snap(r.cue, ['target', 'state', 'lines'])] });
      }
      markChanged();
      var added = correctionOf(r.correctionAdded);
      if (added) { window.VP_Store.set('correctionAdded', added); }
      return r;
    });
  }

  function choose(index, alternative) {
    var c = window.VP_Store.getCue(index);
    if (!c) { return P().reject(new Error('no cue ' + index)); }
    var before = snap(c, ['target', 'state', 'lines']);
    return window.VP_Bridge.call('cue.choose', { index: index, alternative: alternative }).then(function (r) {
      window.VP_Store.putCues([r.cue]);
      window.VP_History.push({ kind: 'choose', labelKey: 'workspace.history.choose.label', indices: [index], before: [before], after: [snap(r.cue, ['target', 'state', 'lines'])] });
      markChanged();
      return r;
    });
  }

  function historyStep(cmd) {
    if (w && w.historyBusy) { return P().resolve(null); }
    if (w) { w.historyBusy = true; }
    var done = function () { if (w) { w.historyBusy = false; } };
    return window.VP_Bridge.call(cmd, {}).then(function (r) {
      var entry = cmd === 'history.undo' ? window.VP_History.undo() : window.VP_History.redo();
      window.VP_History.syncFromEngine(r);
      var changed = r.changedIndices || [];
      var snaps = entry ? (cmd === 'history.undo' ? entry.before : entry.after) : null;
      var work;
      if (snaps && sameSet(entry.indices, changed)) {
        applySnaps(snaps);
        work = P().resolve(0);
      } else {
        work = refetch(changed);
      }
      return work.then(function () {
        if (changed.length) { markChanged(); }
        done();
        return r;
      });
    }).then(null, function (err) {
      done();
      throw err;
    });
  }

  function undo() { return historyStep('history.undo').then(null, showError); }

  function redo() { return historyStep('history.redo').then(null, showError); }

  function untranslatedIndices() {
    var total = window.VP_Store.cueTotal();
    var out = [];
    for (var i = 0; i < total; i++) {
      var c = window.VP_Store.getCue(i);
      if (!c) { return null; }
      if (c.state === 'new') { out.push(i); }
    }
    return out;
  }

  function translate(indices) {
    if (window.VP_Store.get('job')) { return P().resolve(null); }
    var st = settings();
    var params = {
      engines: { rules: true, model: !!(st.engines && st.engines.model), online: !!(st.engines && st.engines.online) },
      // The engine's scale (rules.h): 1 extremely faithful .. 3 flexible, as the slider stores it.
      fidelity: st.defaultFidelity >= 1 && st.defaultFidelity <= 3 ? st.defaultFidelity : 2
    };
    if (indices) { params.indices = indices; }
    window.VP_Store.set('job', { jobId: null, done: 0, total: indices ? indices.length : window.VP_Store.cueTotal(), cuesPerSec: 0, etaSec: 0, starting: true });
    return window.VP_Bridge.call('translate.start', params).then(function (r) {
      var job = window.VP_Store.get('job');
      if (job && job.starting) { window.VP_Store.set('job', copy(job, { jobId: r.jobId, starting: false })); }
      startWarnings(r);
      return r.jobId;
    }, function (err) {
      window.VP_Store.set('job', null);
      showError(err);
      return null;
    });
  }

  // Orbergise the file (PREDESIGN 1.2.3): same job plumbing and events as translate.
  function orbergise(opts) {
    if (window.VP_Store.get('job')) { return P().resolve(null); }
    opts = opts || {};
    var params = { tier: opts.tier === 1 ? 1 : 2, keepNames: opts.keepNames !== false, simplify: opts.simplify !== false };
    if (opts.originalPath) { params.originalPath = opts.originalPath; }
    if (opts.indices) { params.indices = opts.indices; }
    window.VP_Store.set('job', { jobId: null, done: 0, total: opts.indices ? opts.indices.length : window.VP_Store.cueTotal(), cuesPerSec: 0, etaSec: 0, starting: true });
    return window.VP_Bridge.call('orbergise.start', params).then(function (r) {
      var job = window.VP_Store.get('job');
      if (job && job.starting) { window.VP_Store.set('job', copy(job, { jobId: r.jobId, starting: false })); }
      return r.jobId;
    }, function (err) {
      window.VP_Store.set('job', null);
      showError(err);
      return null;
    });
  }

  // After an engine control changed: translated cues (not edited, not reviewed) show the
  // grey dot until "Translate again". The engine has no command for this, so it is UI state
  // until the next translate.start (which skips edited and reviewed cues anyway).
  function markStale() {
    var total = window.VP_Store.cueTotal();
    var out = [];
    for (var i = 0; i < total; i++) {
      var c = window.VP_Store.getCue(i);
      if (c && c.state === 'translated') { out.push(copy(c, { state: 'stale' })); }
    }
    if (out.length) { window.VP_Store.putCues(out); }
    return out.length;
  }

  // ---------------------------------------------------------------- engine warnings
  function engineOfCode(code) { return /^online/.test(String(code || '')) ? 'online' : 'model'; }

  function warnings() { return window.VP_Store.get('warnings') || []; }

  // What the chip and the toast say: "Model unavailable: not installed".
  function warningText(wn) {
    var T = window.VP_I18n;
    var rk = 'warning.reason.' + String(wn.code || '').replace(/_([a-z])/g, function (m, ch) { return ch.toUpperCase(); }) + '.label';
    var engineState = window.VP_Store.get('engine') || {};
    var hm = (engineState.hello && engineState.hello.model) || {};
    // model_missing from a build without engine ii: "not part of this version", not "not installed"
    if (wn.code === 'model_missing' && hm.reason === 'not_built') { rk = 'warning.reason.modelNotBuilt.label'; }
    var reason = T.has(rk) ? T.t(rk) : window.VP_App.errorText(wn.code, wn.hint).title;
    var engine = WARN_ENGINES.indexOf(wn.engine) >= 0 ? wn.engine : engineOfCode(wn.code);
    return T.t('workspace.warning.' + engine + '.label', { reason: reason });
  }

  function openEngines() {
    if (!w) { return; }
    setTab('engines', true);
    if (w.root.getAttribute('data-drawer') !== 'open' && window.innerWidth && window.innerWidth < 1180) { toggleDrawer(true); }
  }

  // One entry per engine; a new engine/code pair of a job is announced once.
  function addWarning(wn, announce) {
    var engine = WARN_ENGINES.indexOf(wn.engine) >= 0 ? wn.engine : engineOfCode(wn.code);
    var entry = { engine: engine, code: wn.code || '', message: wn.message || '', hint: wn.hint || '', jobId: wn.jobId === undefined ? null : wn.jobId };
    var list = warnings().filter(function (x) { return x.engine !== engine; });
    list.push(entry);
    list.sort(function (a, b) { return WARN_ENGINES.indexOf(a.engine) - WARN_ENGINES.indexOf(b.engine); });
    window.VP_Store.set('warnings', list);
    var key = engine + '|' + entry.code + '|' + entry.jobId;
    if (announce !== false && w && !w.announced[key]) {
      w.announced[key] = true;
      window.VP_Toast.show({ text: warningText(entry), kind: 'error', actionKey: 'workspace.warning.open.cta', onAction: openEngines, timeoutMs: 8000 });
    }
    return entry;
  }

  // translate.start answered: its warning codes are the state of this job (the events that
  // follow add the hint); a job without warnings clears the chips of the previous one.
  function startWarnings(r) {
    var codes = (r && r.warnings) || [];
    window.VP_Store.set('warnings', []);
    for (var i = 0; i < codes.length; i++) { addWarning({ code: codes[i], jobId: r.jobId }, true); }
  }

  function onWarning(e) {
    if (!e) { return; }
    addWarning(e, true);
  }

  function cancel() {
    var job = window.VP_Store.get('job');
    if (!job || !job.jobId) { return P().resolve(false); }
    return window.VP_Bridge.call('translate.cancel', { jobId: job.jobId }).then(function () { return true; }, function (err) {
      showError(err);
      return false;
    });
  }

  // ---------------------------------------------------------------- translation events
  function ours(e) {
    var job = window.VP_Store.get('job');
    return !!job && (job.jobId === null || job.jobId === e.jobId);
  }

  function onProgress(e) {
    if (!ours(e)) { return; }
    window.VP_Store.set('job', { jobId: e.jobId, done: e.done, total: e.total, cuesPerSec: e.cuesPerSec, etaSec: e.etaSec, starting: false });
  }

  function onCues(e) {
    if (!ours(e)) { return; }
    var list = e.cues || (e.cue ? [e.cue] : []);
    if (list.length) { window.VP_Store.putCues(list); }
  }

  function onDone(e) {
    if (!ours(e)) { return; }
    window.VP_Store.set('job', null);
    markChanged();
    var later = e.warnings || (e.stats && e.stats.warnings) || [];
    for (var i = 0; i < later.length; i++) { addWarning(typeof later[i] === 'string' ? { code: later[i], jobId: e.jobId } : later[i], true); }
    var stats = e.stats || {};
    window.VP_Toast.show({
      key: stats.cancelled ? 'workspace.job.cancelled.label' : 'workspace.job.done.label',
      vars: { n: stats.translated || 0, ok: stats.ok || 0, check: stats.check || 0, fix: stats.fix || 0 },
      kind: 'success'
    });
    if (!window.VP_CueList.isMounted()) { return; }
    var first = window.VP_CueList.firstReview();
    if (first >= 0) {
      window.VP_CueList.setFilter('review');
      window.VP_CueList.select(first);
    } else {
      window.VP_CueList.setFilter('all');
    }
  }

  function onError(e) {
    if (!ours(e)) { return; }
    window.VP_Store.set('job', null);
    showError({ code: e.code, hint: e.hint });
  }

  // ---------------------------------------------------------------- rendering
  function pairKey(pair) {
    return window.VP_Start && typeof window.VP_Start.pairLabelKey === 'function' ? window.VP_Start.pairLabelKey(pair) : 'start.pair.enLa.label';
  }

  function renderSave() {
    if (!w) { return; }
    var st = window.VP_Store.get('saveState') || { kind: 'unsaved' };
    var key = {
      saved: 'workspace.save.saved.label',
      autosaved: 'workspace.save.autosaved.label',
      pending: settings().autosave === false ? 'workspace.save.pendingOff.label' : 'workspace.save.pending.label'
    }[st.kind] || 'workspace.save.never.label';
    var vars = st.at && (st.kind === 'saved' || st.kind === 'autosaved') ? { time: clock(st.at) } : null;
    setText(w.saveEl, key, vars);
    setText(w.autosaveEl, key, vars);
    w.saveEl.setAttribute('data-state', st.kind);
  }

  function renderCounts() {
    if (!w) { return; }
    var c = window.VP_Store.get('cueCounts') || {};
    var total = window.VP_Store.cueTotal();
    setText(w.countEls[0], 'workspace.status.translated.label', { done: c.translated || 0, total: total });
    setText(w.countEls[1], 'workspace.status.check.label', { n: c.check || 0 });
    setText(w.countEls[2], 'workspace.status.fix.label', { n: c.fix || 0 });
    renderJob();
  }

  function renderEngines() {
    if (!w) { return; }
    var st = settings();
    var model = !!(st.engines && st.engines.model);
    var online = !!(st.engines && st.engines.online);
    setText(w.engineEls[0], 'workspace.status.rules.label');
    setText(w.engineEls[1], model ? 'workspace.status.model.on.label' : 'workspace.status.model.off.label');
    setText(w.engineEls[2], online ? 'workspace.status.online.on.label' : 'workspace.status.online.off.label');
    var D = window.VP_Dom;
    D.clear(w.netEl);
    w.netEl.className = 'vp-net ' + (online ? 'vp-net-online' : 'vp-net-offline');
    D.append(w.netEl, [useIcon(online ? '#vp-i-online' : '#vp-i-offline'), D.el('span', { 'data-i18n': online ? 'app.network.online.label' : 'app.network.offline.label' })]);
    window.VP_I18n.bind(w.netEl);
  }

  function renderWarnings() {
    if (!w) { return; }
    var D = window.VP_Dom;
    var list = warnings();
    D.clear(w.warnEl);
    w.warnEl.hidden = !list.length;
    list.forEach(function (wn) {
      var text = warningText(wn);
      w.warnEl.appendChild(D.el('button', { type: 'button', className: 'vp-warn-chip vp-ws-warning', title: wn.hint || text, dataset: { wsAction: 'warnings', warnEngine: wn.engine } }, [
        D.el('span', { text: text })
      ]));
    });
  }

  function renderHistory() {
    if (!w) { return; }
    var H = window.VP_History;
    w.undoBtn.disabled = !H.canUndo();
    w.redoBtn.disabled = !H.canRedo();
    var u = H.peekUndo();
    var r = H.peekRedo();
    w.undoBtn.setAttribute('title', u && u.labelKey ? window.VP_I18n.t('workspace.undo.tooltip', { what: window.VP_I18n.t(u.labelKey) }) : window.VP_I18n.t('key.undo.label'));
    w.redoBtn.setAttribute('title', r && r.labelKey ? window.VP_I18n.t('workspace.redo.tooltip', { what: window.VP_I18n.t(r.labelKey) }) : window.VP_I18n.t('key.redo.label'));
  }

  function renderJob() {
    if (!w) { return; }
    var job = window.VP_Store.get('job');
    var running = !!job;
    w.jobIdle.hidden = running;
    w.jobRun.hidden = !running;
    if (running) {
      var pct = job.total ? Math.round(100 * job.done / job.total) : 0;
      w.jobBar.style.width = pct + '%';
      w.jobProgress.setAttribute('aria-valuenow', String(pct));
      var eta = job.etaSec || 0;
      setText(w.jobText, job.starting ? 'workspace.job.starting.label' : 'workspace.job.progress.label', {
        done: job.done, total: job.total, rate: Math.round((job.cuesPerSec || 0) * 10) / 10,
        eta: eta >= 60 ? window.VP_I18n.t('unit.minutes', { n: Math.round(eta / 60) }) : window.VP_I18n.t('unit.seconds', { n: Math.max(1, Math.round(eta)) })
      });
      w.cancelBtn.disabled = !job.jobId;
      return;
    }
    var c = window.VP_Store.get('cueCounts') || {};
    var total = window.VP_Store.cueTotal();
    var translated = c.translated || 0;
    if (!translated) {
      setText(w.translateBtn, 'workspace.translate.all.cta', { n: total });
      w.translateBtn.setAttribute('data-scope', 'all');
    } else if (translated < total) {
      setText(w.translateBtn, 'workspace.translate.rest.cta', { n: total - translated });
      w.translateBtn.setAttribute('data-scope', 'rest');
    } else {
      setText(w.translateBtn, 'workspace.translate.again.cta');
      w.translateBtn.setAttribute('data-scope', 'all');
    }
    w.translateBtn.disabled = !total;
    var sel = window.VP_Store.get('selection');
    w.translateOneBtn.disabled = !sel;
  }

  function setMode(mode) {
    if (!w || (mode !== 'translate' && mode !== 'orberg')) { return false; }
    w.mode = mode;
    var tr = mode === 'translate';
    w.tabTranslate.setAttribute('aria-selected', tr ? 'true' : 'false');
    w.tabOrberg.setAttribute('aria-selected', tr ? 'false' : 'true');
    w.tabTranslate.tabIndex = tr ? 0 : -1;
    w.tabOrberg.tabIndex = tr ? -1 : 0;
    w.translatePanel.hidden = !tr;
    w.orbergPanel.hidden = tr;
    var O = window.VP_Orberg;
    if (O) {
      if (!tr && !O.isMounted()) { O.mount(w.orbergPanel); } else if (tr && O.isMounted()) { O.destroy(); }
    }
    return true;
  }

  function toggleDrawer(open) {
    if (!w) { return false; }
    var want = open === undefined ? w.root.getAttribute('data-drawer') !== 'open' : !!open;
    w.root.setAttribute('data-drawer', want ? 'open' : 'closed');
    w.drawerBtn.setAttribute('aria-expanded', want ? 'true' : 'false');
    if (want) { w.right.focus(); }
    return want;
  }

  function openExport() {
    if (window.VP_Export) { window.VP_Export.open(); }
  }

  function openSettings() {
    if (window.VP_Settings) { window.VP_Settings.open(); }
  }

  function openHelp() {
    if (window.VP_App && typeof window.VP_App.openHelp === 'function') { window.VP_App.openHelp(); } else if (window.VP_App) { window.VP_App.openShortcuts(); }
  }

  // ---------------------------------------------------------------- right panel tabs
  function tabById(id) {
    for (var i = 0; i < PANEL_TABS.length; i++) { if (PANEL_TABS[i].id === id) { return PANEL_TABS[i]; } }
    return null;
  }

  function setTab(id, focus) {
    if (!w || !w.tabBtns) { return false; }
    var tab = tabById(id) || PANEL_TABS[0];
    var cur = w.tab ? tabById(w.tab) : null;
    if (cur && cur.id !== tab.id && window[cur.module] && window[cur.module].isMounted && window[cur.module].isMounted()) { window[cur.module].destroy(); }
    window.VP_Dom.clear(w.panelBody);
    w.tab = tab.id;
    PANEL_TABS.forEach(function (t) {
      var b = w.tabBtns[t.id];
      var on = t.id === tab.id;
      b.setAttribute('aria-selected', on ? 'true' : 'false');
      b.tabIndex = on ? 0 : -1;
    });
    w.panelBody.setAttribute('aria-labelledby', 'vp-ptab-' + tab.id);
    var mod = window[tab.module];
    if (mod && typeof mod.mount === 'function') {
      mod.mount(w.panelBody);
    } else {
      w.panelBody.appendChild(window.VP_Dom.el('p', { className: 'vp-hint', 'data-i18n': 'workspace.panel.empty' }));
      window.VP_I18n.bind(w.panelBody);
    }
    if (focus) { w.tabBtns[tab.id].focus(); }
    if (settings().panelTab !== tab.id && window.VP_App && typeof window.VP_App.saveSettings === 'function') { window.VP_App.saveSettings({ panelTab: tab.id }); }
    return true;
  }

  function onPanelTabKey(e) {
    if (!w) { return; }
    var ids = PANEL_TABS.map(function (t) { return t.id; });
    var i = ids.indexOf(w.tab);
    var next = null;
    if (e.key === 'ArrowRight') { next = ids[(i + 1) % ids.length]; } else if (e.key === 'ArrowLeft') { next = ids[(i + ids.length - 1) % ids.length]; } else if (e.key === 'Home') { next = ids[0]; } else if (e.key === 'End') { next = ids[ids.length - 1]; }
    if (!next) { return; }
    e.preventDefault();
    setTab(next, true);
  }

  function destroyPanel() {
    if (!w || !w.tab) { return; }
    var cur = tabById(w.tab);
    if (cur && window[cur.module] && window[cur.module].isMounted && window[cur.module].isMounted()) { window[cur.module].destroy(); }
    w.tab = null;
  }

  // An inspected word switches to the Word tab (and opens the drawer below 1180 px).
  function onInspect(x) {
    if (!w || !x) { return; }
    if (w.tab !== 'word') { setTab('word'); }
    if (w.root.getAttribute('data-drawer') !== 'open' && window.innerWidth && window.innerWidth < 1180) { toggleDrawer(true); }
  }

  // ---------------------------------------------------------------- save and close
  function save() {
    var p = window.VP_Store.get('project');
    if (!p) { return P().resolve(null); }
    var B = window.VP_Bridge;
    var go = p.path ? B.call('project.save', {}) : B.call('dialog.saveFile', { suggestedName: String(p.name || 'project').replace(/\.[^.]+$/, '') + '.vpoeta' }).then(function (r) {
      if (!r || !r.path) { return null; }
      return B.call('project.saveAs', { path: r.path });
    });
    return go.then(function (r) {
      if (!r) { return null; }
      var cur = window.VP_Store.get('project');
      if (cur) { window.VP_Store.set('project', copy(cur, { path: r.path })); }
      window.VP_Store.set('saveState', { kind: 'saved', at: r.at ? new Date(r.at).getTime() : new Date().getTime() });
      if (window.VP_Start && typeof window.VP_Start.remember === 'function') { window.VP_Start.remember(window.VP_Store.get('project'), window.VP_Store.get('cueCounts')); }
      window.VP_Toast.show({ key: 'workspace.save.done.label', kind: 'success' });
      return r.path;
    }, function (err) {
      showError(err);
      return null;
    });
  }

  function close() {
    var p = window.VP_Store.get('project');
    var counts = window.VP_Store.get('cueCounts');
    var job = window.VP_Store.get('job');
    var B = window.VP_Bridge;
    if (w) { w.closing = true; }
    var first = job && job.jobId ? B.call('translate.cancel', { jobId: job.jobId }).then(null, function () { return null; }) : P().resolve(null);
    return first.then(function () {
      return B.call('project.close', {}).then(null, function () { return null; });
    }).then(function () {
      if (p && window.VP_Start && typeof window.VP_Start.remember === 'function') { window.VP_Start.remember(p, counts); }
      window.VP_History.clear();
      window.VP_Store.set('selection', null);
      window.VP_Store.set('inspect', null);
      window.VP_Store.set('cueCounts', null);
      window.VP_Store.set('saveState', null);
      window.VP_Store.reset();
      return true;
    });
  }

  // ---------------------------------------------------------------- keyboard
  function interactive(e) {
    var t = e && e.target;
    return !!(t && t.nodeType === 1 && t !== w.root && window.VP_Dom.closest(t, INTERACTIVE, w.root) && !window.VP_Dom.matches(t, '.vp-cl-viewport'));
  }

  function typing(e) { return !!(e && window.VP_Keys.isTyping(e.target)); }

  function selectedIndex() {
    var sel = window.VP_Store.get('selection');
    return sel ? sel.index : null;
  }

  function acceptSelected() {
    var i = selectedIndex();
    if (i === null) { return P().resolve(0); }
    return review([i], true, { labelKey: 'workspace.history.accept.label' }).then(null, function (err) {
      showError(err);
      return 0;
    });
  }

  function acceptAndNext() {
    return acceptSelected().then(function () {
      if (window.VP_CueList.nextReview(1) < 0) {
        window.VP_Toast.show({ key: 'workspace.review.allDone.label', actionKey: 'workspace.review.export.cta', onAction: openExport });
      }
    });
  }

  function toggleView(field) {
    var st = settings();
    var patch = {};
    patch[field] = st[field] === false;
    window.VP_Store.patch('settings', patch);
    if (window.VP_App && typeof window.VP_App.saveSettings === 'function') { window.VP_App.saveSettings(patch); }
    window.VP_Toast.show({ key: 'workspace.view.' + field + '.' + (patch[field] ? 'on' : 'off') + '.label' });
  }

  function keyHandlers() {
    var Pn = window.VP_Panes;
    var L = window.VP_CueList;
    return {
      nextCue: function () { L.next(); },
      prevCue: function () { L.prev(); },
      nextReview: function (e) {
        if (typing(e)) { return false; }
        if (L.nextReview(1) < 0) { window.VP_Toast.show({ key: 'workspace.review.none.label' }); }
      },
      prevReview: function (e) {
        if (typing(e)) { return false; }
        if (L.nextReview(-1) < 0) { window.VP_Toast.show({ key: 'workspace.review.none.label' }); }
      },
      accept: function (e) {
        if (interactive(e) || Pn.mode() !== 'view') { return false; }
        acceptSelected();
      },
      acceptNext: function (e) {
        if (interactive(e) || Pn.mode() !== 'view') { return false; }
        acceptAndNext();
      },
      edit: function (e) {
        if (w.mode !== 'translate' || typing(e)) { return false; }
        return Pn.startEdit() ? undefined : false;
      },
      cancelEdit: function () {
        if (Pn.mode() === 'edit') { Pn.cancelEdit(); return undefined; }
        if (Pn.dismiss()) { return undefined; }
        if (w.root.getAttribute('data-drawer') === 'open') {
          toggleDrawer(false);
          w.drawerBtn.focus();
          return undefined;
        }
        return false;
      },
      acceptEdit: function () {
        if (Pn.mode() !== 'edit') { return false; }
        Pn.acceptEdit();
      },
      alt1: function () { return Pn.chooseAlt(1) ? undefined : false; },
      alt2: function () { return Pn.chooseAlt(2) ? undefined : false; },
      alt3: function () { return Pn.chooseAlt(3) ? undefined : false; },
      search: function () { L.focusSearch(); },
      macrons: function () { toggleView('showMacrons'); },
      emoji: function () { toggleView('showEmoji'); },
      interlinear: function () { return Pn.toggleInterlinear && Pn.toggleInterlinear() !== null ? undefined : false; },
      undo: function (e) {
        if (typing(e)) { return false; }
        undo();
      },
      redo: function (e) {
        if (typing(e)) { return false; }
        redo();
      },
      save: function () { save(); },
      'export': function () { openExport(); },
      settings: function () { openSettings(); }
    };
  }

  // ---------------------------------------------------------------- build
  function build(root, project) {
    var D = window.VP_Dom;
    var tr = function (id, key, extra) {
      var a = { id: id, type: 'button', role: 'tab', className: 'vp-tab', 'data-i18n': key, 'aria-controls': id + '-panel' };
      if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
      return D.el('button', a);
    };
    w.saveEl = D.el('span', { className: 'vp-ws-save' });
    w.autosaveEl = D.el('span', { className: 'vp-ws-autosave', 'aria-live': 'polite' });
    w.countEls = [D.el('span'), D.el('span'), D.el('span')];
    w.engineEls = [D.el('span'), D.el('span'), D.el('span')];
    w.netEl = D.el('span', { className: 'vp-net' });
    w.warnEl = D.el('span', { className: 'vp-ws-warnings', 'aria-live': 'polite', hidden: true });
    w.undoBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-ws-undo', dataset: { wsAction: 'undo' } }, [svgIcon('M6 4L2.5 7.5L6 11M3 7.5h6.5a4 4 0 0 1 0 8H8'), D.el('span', { 'data-i18n': 'workspace.undo.cta' })]);
    w.redoBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-ws-redo', dataset: { wsAction: 'redo' } }, [svgIcon('M10 4l3.5 3.5L10 11M13 7.5H6.5a4 4 0 0 0 0 8H8'), D.el('span', { 'data-i18n': 'workspace.redo.cta' })]);
    w.drawerBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon vp-ws-drawer-btn', 'aria-expanded': 'false', 'aria-controls': 'vp-ws-panel', 'data-i18n-aria': 'workspace.drawer.aria', 'data-i18n-title': 'workspace.drawer.aria', dataset: { wsAction: 'drawer' } }, [svgIcon('M2.5 3h11v10h-11zM10 3v10')]);
    w.tabTranslate = tr('vp-tab-translate', 'workspace.mode.translate.label');
    w.tabOrberg = tr('vp-tab-orberg', 'workspace.mode.orberg.label');
    w.translateBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', id: 'vp-translate', dataset: { wsAction: 'translate' } });
    w.translateOneBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'workspace.translate.one.cta', dataset: { wsAction: 'translateOne' } });
    w.jobBar = D.el('div', { className: 'vp-progress-bar' });
    w.jobProgress = D.el('div', { className: 'vp-progress vp-job-progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': '0', 'data-i18n-aria': 'workspace.job.aria' }, [w.jobBar]);
    w.jobText = D.el('p', { className: 'vp-job-text', 'aria-live': 'polite' });
    w.cancelBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'workspace.job.cancel.cta', dataset: { wsAction: 'cancel' } });
    w.jobIdle = D.el('div', { className: 'vp-job-idle' }, [w.translateBtn, w.translateOneBtn]);
    w.jobRun = D.el('div', { className: 'vp-job-run', hidden: true }, [w.jobProgress, w.jobText, w.cancelBtn]);
    w.left = D.el('nav', { className: 'vp-ws-left', 'data-i18n-aria': 'workspace.left.aria' });
    w.panes = D.el('div', { className: 'vp-ws-panes' });
    w.translatePanel = D.el('div', { id: 'vp-tab-translate-panel', className: 'vp-ws-tabpanel', role: 'tabpanel', 'aria-labelledby': 'vp-tab-translate' }, [
      D.el('div', { className: 'vp-ws-job' }, [w.jobIdle, w.jobRun]),
      w.panes
    ]);
    w.orbergPanel = D.el('div', { id: 'vp-tab-orberg-panel', className: 'vp-ws-tabpanel', role: 'tabpanel', 'aria-labelledby': 'vp-tab-orberg', hidden: true });
    w.tabBtns = {};
    w.panelBody = D.el('div', { id: 'vp-panel-body', className: 'vp-panel-body', role: 'tabpanel' });
    w.host = D.el('div', { id: 'vp-panel-host', className: 'vp-panel-host' }, [
      D.el('div', { className: 'vp-tabs vp-panel-tabs', role: 'tablist', 'data-i18n-aria': 'workspace.panel.aria' }, PANEL_TABS.map(function (t) {
        w.tabBtns[t.id] = D.el('button', { id: 'vp-ptab-' + t.id, type: 'button', role: 'tab', className: 'vp-tab vp-ptab', 'aria-selected': 'false', 'aria-controls': 'vp-panel-body', tabIndex: -1, 'data-i18n': t.key, dataset: { panelTab: t.id } });
        return w.tabBtns[t.id];
      })),
      w.panelBody
    ]);
    w.exportBtn = D.el('button', { id: 'vp-export-btn', type: 'button', className: 'vp-btn vp-btn-secondary vp-ws-export', 'aria-keyshortcuts': 'Control+Shift+E', dataset: { wsAction: 'export' } }, [
      useIcon('img/icons.svg#export'), D.el('span', { 'data-i18n': 'workspace.export.cta' })
    ]);
    w.right = D.el('section', { id: 'vp-ws-panel', className: 'vp-ws-right', tabIndex: -1, 'data-i18n-aria': 'workspace.panel.aria' }, [w.host]);
    var lang = String(project.pair || '').indexOf('grc') >= 0;
    w.greek = lang;
    w.root = D.el('div', { className: 'vp-ws' + (lang ? ' pair-grc' : ''), 'data-drawer': 'closed', 'data-kind': project.kind || 'subs', 'data-pair': project.pair || '' }, [
      D.el('header', { className: 'vp-ws-top' }, [
        D.el('button', { type: 'button', className: 'vp-ws-home', 'data-i18n-aria': 'workspace.home.aria', 'data-i18n-title': 'workspace.home.aria', dataset: { wsAction: 'home' } }, [
          window.VP_App && typeof window.VP_App.lockup === 'function' ? window.VP_App.lockup() : D.el('span', { className: 'vp-wordmark', 'data-i18n': 'app.name' })
        ]),
        D.el('div', { className: 'vp-ws-project' }, [
          D.el('span', { className: 'vp-ws-name', text: project.name || '', title: project.path || project.sourcePath || '' }),
          w.saveEl
        ]),
        D.el('span', { className: 'vp-ws-pair', 'data-i18n': pairKey(project.pair) }),
        D.el('div', { className: 'vp-tabs vp-ws-tabs', role: 'tablist', 'data-i18n-aria': 'workspace.mode.aria' }, [w.tabTranslate, w.tabOrberg]),
        D.el('span', { className: 'vp-spacer' }),
        w.undoBtn,
        w.redoBtn,
        w.exportBtn,
        w.drawerBtn,
        D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon', 'data-i18n-aria': 'workspace.settings.aria', 'data-i18n-title': 'workspace.settings.aria', dataset: { wsAction: 'settings' } }, [
          svgIcon('M8 5.5a2.5 2.5 0 1 0 0 5a2.5 2.5 0 1 0 0-5zM8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2M3.4 3.4l1.4 1.4M11.2 11.2l1.4 1.4M3.4 12.6l1.4-1.4M11.2 4.8l1.4-1.4')
        ]),
        D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon', 'data-i18n-aria': 'workspace.help.aria', 'data-i18n-title': 'workspace.help.aria', dataset: { wsAction: 'help' } }, [
          svgIcon('M8 14.5a6.5 6.5 0 1 0 0-13a6.5 6.5 0 1 0 0 13zM6 6.2a2 2 0 1 1 2.6 1.9c-.4.2-.6.5-.6.9v.8M8 11.5v.1')
        ])
      ]),
      D.el('div', { className: 'vp-ws-body' }, [
        w.left,
        D.el('div', { className: 'vp-ws-centre' }, [w.translatePanel, w.orbergPanel]),
        w.right
      ]),
      D.el('footer', { className: 'vp-ws-status' }, [
        w.autosaveEl,
        D.el('span', { className: 'vp-ws-counts vp-status-line' }, w.countEls),
        D.el('span', { className: 'vp-spacer' }),
        w.warnEl,
        D.el('span', { className: 'vp-ws-engines vp-status-line' }, w.engineEls),
        w.netEl
      ])
    ]);
    root.appendChild(w.root);
  }

  function onAction(e, btn) {
    var a = btn.getAttribute('data-ws-action');
    if (a === 'undo') { undo(); } else if (a === 'redo') { redo(); } else if (a === 'drawer') { toggleDrawer(); } else if (a === 'settings') {
      openSettings();
    } else if (a === 'help') {
      openHelp();
    } else if (a === 'export') {
      openExport();
    } else if (a === 'home') {
      close();
    } else if (a === 'translate') {
      var scope = btn.getAttribute('data-scope');
      translate(scope === 'rest' ? untranslatedIndices() : null);
    } else if (a === 'translateOne') {
      var i = selectedIndex();
      if (i !== null) { translate([i]); }
    } else if (a === 'cancel') {
      cancel();
    } else if (a === 'warnings') {
      openEngines();
    }
  }

  function onTabKey(e) {
    if (window.VP_Dom.closest(e.target, '.vp-ptab', w.root)) {
      onPanelTabKey(e);
      return;
    }
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') { return; }
    e.preventDefault();
    var next = w.mode === 'translate' ? 'orberg' : 'translate';
    setMode(next);
    (next === 'translate' ? w.tabTranslate : w.tabOrberg).focus();
  }

  function mount(root, params) {
    if (w) { destroy(); }
    params = params || {};
    var D = window.VP_Dom;
    var project = window.VP_Store.get('project');
    w = { mode: 'translate', removers: [], historyBusy: false, closing: false, appEl: null, announced: {}, greek: false };
    if (!project) {
      w.root = D.el('section', { className: 'vp-ws-none vp-card' }, [
        D.el('h1', { 'data-i18n': 'workspace.none.title' }),
        D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', 'data-i18n': 'workspace.none.cta', dataset: { wsAction: 'home' } })
      ]);
      root.appendChild(w.root);
      D.delegate(w.root, '[data-ws-action]', 'click', function () { window.VP_Store.reset(); }, { owner: OWNER });
      return;
    }
    build(root, project);
    w.appEl = root.parentNode && root.parentNode.classList && root.parentNode.classList.contains('vp-app') ? root.parentNode : null;
    if (w.appEl) {
      w.appEl.classList.add('vp-app-ws');
      if (w.greek) { w.appEl.classList.add('pair-grc'); }
    }
    D.delegate(w.root, '[data-ws-action]', 'click', onAction, { owner: OWNER });
    D.delegate(w.root, '[role="tab"]', 'click', function (e, tab) {
      var pt = tab.getAttribute('data-panel-tab');
      if (pt) { setTab(pt); } else { setMode(tab === w.tabOrberg ? 'orberg' : 'translate'); }
    }, { owner: OWNER });
    D.delegate(w.root, '[role="tab"]', 'keydown', onTabKey, { owner: OWNER });

    var S = window.VP_Store;
    if (!S.get('saveState')) { S.set('saveState', project.path ? { kind: 'saved', at: new Date().getTime() } : { kind: 'unsaved', at: 0 }); }
    w.removers.push(S.subscribe('saveState', renderSave));
    w.removers.push(S.subscribe('cueCounts', renderCounts));
    w.removers.push(S.subscribe('settings', renderEngines));
    w.removers.push(S.subscribe('job', renderJob));
    w.removers.push(S.subscribe('selection', renderJob));
    w.removers.push(S.subscribe('inspect', onInspect));
    w.removers.push(S.subscribe('warnings', renderWarnings));
    w.removers.push(window.VP_History.onChange(renderHistory));
    w.removers.push(window.VP_I18n.onLanguageChanged(function () {
      renderHistory();
      renderJob();
      renderWarnings();
    }));
    var B = window.VP_Bridge;
    w.removers.push(B.on('translate.warning', onWarning));
    w.removers.push(B.on('translate.progress', onProgress));
    w.removers.push(B.on('translate.cue', onCues));
    w.removers.push(B.on('translate.done', onDone));
    w.removers.push(B.on('translate.error', onError));
    w.removers.push(B.on('project.autosaved', onAutosaved));
    var handlers = keyHandlers();
    KEY_ACTIONS.forEach(function (action) { w.removers.push(window.VP_Keys.handle(action, handlers[action], OWNER)); });

    S.setCueTotal(project.cues || 0);
    if (S.get('warnings')) { S.set('warnings', null); }
    window.VP_CueList.mount(w.left, { total: project.cues || 0, kind: project.kind, pair: project.pair });
    window.VP_Panes.mount(w.panes, { kind: project.kind, pair: project.pair });
    w.tab = null;
    setTab(params.tab || (tabById(settings().panelTab) ? settings().panelTab : 'word'));
    setMode(params.mode || (project.pair === 'la-la' ? 'orberg' : 'translate'));
    renderSave();
    renderCounts();
    renderEngines();
    renderWarnings();
    renderHistory();
    renderJob();
    window.VP_I18n.bind(w.root);
    if ((project.cues || 0) > 0) { window.VP_CueList.select(typeof params.select === 'number' ? params.select : 0); }
  }

  function destroy() {
    if (!w) { return; }
    destroyPanel();
    if (window.VP_Orberg && window.VP_Orberg.isMounted()) { window.VP_Orberg.destroy(); }
    window.VP_Panes.destroy();
    window.VP_CueList.destroy();
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (w.removers.length) { w.removers.pop()(); }
    if (w.appEl) {
      w.appEl.classList.remove('vp-app-ws');
      w.appEl.classList.remove('pair-grc');
    }
    if (w.root && w.root.parentNode) { w.root.parentNode.removeChild(w.root); }
    w = null;
  }

  function i18nKeys() {
    var keys = ['workspace.view.showMacrons.on.label', 'workspace.view.showMacrons.off.label', 'workspace.view.showEmoji.on.label', 'workspace.view.showEmoji.off.label',
      'workspace.save.saved.label', 'workspace.save.autosaved.label', 'workspace.save.never.label', 'workspace.save.pending.label', 'workspace.save.pendingOff.label', 'workspace.undo.tooltip', 'workspace.redo.tooltip',
      'workspace.job.progress.label', 'workspace.job.done.label', 'workspace.job.cancelled.label', 'workspace.translate.all.cta', 'workspace.translate.rest.cta',
      'unit.minutes', 'unit.seconds', 'workspace.warning.model.label', 'workspace.warning.online.label'];
    ['modelMissing', 'modelNotBuilt', 'modelUnsupportedCpu', 'modelLoadFailed', 'onlineDisabled', 'onlineFailed'].forEach(function (c) { keys.push('warning.reason.' + c + '.label'); });
    return keys;
  }

  window.VP_Workspace = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return w !== null; },
    panelHost: function () { return w ? w.host : null; },
    mode: function () { return w ? w.mode : null; },
    setMode: setMode,
    toggleDrawer: toggleDrawer,
    drawerOpen: function () { return !!w && w.root.getAttribute('data-drawer') === 'open'; },
    save: save,
    close: close,
    cmd: { review: review, edit: edit, choose: choose, undo: undo, redo: redo, translate: translate, orbergise: orbergise, cancel: cancel, refetch: refetch, markStale: markStale },
    setTab: setTab,
    tab: function () { return w ? w.tab : null; },
    warnings: warnings,
    warningText: warningText,
    openEngines: openEngines,
    PANEL_TABS: PANEL_TABS.slice(),
    keyActions: function () { return KEY_ACTIONS.slice(); },
    i18nKeys: i18nKeys
  };
}());
