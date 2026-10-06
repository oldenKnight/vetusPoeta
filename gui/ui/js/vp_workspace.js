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
 *   remember), choose(index, alternative), undo(), redo(), translate(indices?), cancel(),
 *   refetch(indices) -> Promise
 * Store keys it writes: selection {index}, job, saveState {kind, at}, inspect (VP_Panes),
 * cueCounts (VP_CueList).
 */
(function () {
  'use strict';

  var OWNER = 'workspace';
  var PAGE = 200;
  var KEY_ACTIONS = ['nextCue', 'prevCue', 'nextReview', 'prevReview', 'accept', 'acceptNext', 'edit', 'cancelEdit', 'acceptEdit',
    'alt1', 'alt2', 'alt3', 'search', 'macrons', 'emoji', 'undo', 'redo', 'save', 'export', 'settings'];
  var INTERACTIVE = 'button, a, input, select, textarea, [role="button"], [role="tab"], [role="option"]';

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
      fidelity: st.defaultFidelity || 2
    };
    if (indices) { params.indices = indices; }
    window.VP_Store.set('job', { jobId: null, done: 0, total: indices ? indices.length : window.VP_Store.cueTotal(), cuesPerSec: 0, etaSec: 0, starting: true });
    return window.VP_Bridge.call('translate.start', params).then(function (r) {
      var job = window.VP_Store.get('job');
      if (job && job.starting) { window.VP_Store.set('job', copy(job, { jobId: r.jobId, starting: false })); }
      return r.jobId;
    }, function (err) {
      window.VP_Store.set('job', null);
      showError(err);
      return null;
    });
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

  function placeholderDialog(titleKey, textKey) {
    return window.VP_Dialog.open({ titleKey: titleKey, textKey: textKey, actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }] });
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
        window.VP_Toast.show({ key: 'workspace.review.allDone.label', actionKey: 'workspace.review.export.cta', onAction: function () { placeholderDialog('workspace.export.title', 'workspace.export.soon.text'); } });
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
      undo: function (e) {
        if (typing(e)) { return false; }
        undo();
      },
      redo: function (e) {
        if (typing(e)) { return false; }
        redo();
      },
      save: function () { save(); },
      'export': function () { placeholderDialog('workspace.export.title', 'workspace.export.soon.text'); },
      settings: function () { placeholderDialog('workspace.settings.title', 'workspace.settings.soon.text'); }
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
    w.orbergPanel = D.el('div', { id: 'vp-tab-orberg-panel', className: 'vp-ws-tabpanel', role: 'tabpanel', 'aria-labelledby': 'vp-tab-orberg', hidden: true }, [
      D.el('div', { className: 'vp-card vp-ws-soon' }, [
        D.el('h2', { 'data-i18n': 'workspace.orberg.soon.title' }),
        D.el('p', { className: 'vp-hint', 'data-i18n': 'workspace.orberg.soon.text' })
      ])
    ]);
    w.host = D.el('div', { id: 'vp-panel-host', className: 'vp-panel-host' }, [
      D.el('div', { className: 'vp-panel-empty' }, [
        D.el('h2', { 'data-i18n': 'workspace.panel.title' }),
        D.el('p', { className: 'vp-hint', 'data-i18n': 'workspace.panel.empty' })
      ])
    ]);
    w.right = D.el('section', { id: 'vp-ws-panel', className: 'vp-ws-right', tabIndex: -1, 'data-i18n-aria': 'workspace.panel.aria' }, [w.host]);
    var lang = String(project.pair || '').indexOf('grc') >= 0;
    w.root = D.el('div', { className: 'vp-ws' + (lang ? ' pair-grc' : ''), 'data-drawer': 'closed', 'data-kind': project.kind || 'subs' }, [
      D.el('header', { className: 'vp-ws-top' }, [
        D.el('button', { type: 'button', className: 'vp-ws-home', 'data-i18n-aria': 'workspace.home.aria', 'data-i18n-title': 'workspace.home.aria', dataset: { wsAction: 'home' } }, [
          D.el('span', { className: 'vp-wordmark', 'data-i18n': 'app.name' })
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
        D.el('span', { className: 'vp-ws-engines vp-status-line' }, w.engineEls),
        w.netEl
      ])
    ]);
    root.appendChild(w.root);
  }

  function onAction(e, btn) {
    var a = btn.getAttribute('data-ws-action');
    if (a === 'undo') { undo(); } else if (a === 'redo') { redo(); } else if (a === 'drawer') { toggleDrawer(); } else if (a === 'settings') {
      placeholderDialog('workspace.settings.title', 'workspace.settings.soon.text');
    } else if (a === 'help') {
      if (window.VP_App && typeof window.VP_App.openShortcuts === 'function') { window.VP_App.openShortcuts(); }
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
    }
  }

  function onTabKey(e) {
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
    w = { mode: 'translate', removers: [], historyBusy: false, closing: false, appEl: null };
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
    if (w.appEl) { w.appEl.classList.add('vp-app-ws'); }
    D.delegate(w.root, '[data-ws-action]', 'click', onAction, { owner: OWNER });
    D.delegate(w.root, '[role="tab"]', 'click', function (e, tab) { setMode(tab === w.tabOrberg ? 'orberg' : 'translate'); }, { owner: OWNER });
    D.delegate(w.root, '[role="tab"]', 'keydown', onTabKey, { owner: OWNER });

    var S = window.VP_Store;
    if (!S.get('saveState')) { S.set('saveState', project.path ? { kind: 'saved', at: new Date().getTime() } : { kind: 'unsaved', at: 0 }); }
    w.removers.push(S.subscribe('saveState', renderSave));
    w.removers.push(S.subscribe('cueCounts', renderCounts));
    w.removers.push(S.subscribe('settings', renderEngines));
    w.removers.push(S.subscribe('job', renderJob));
    w.removers.push(S.subscribe('selection', renderJob));
    w.removers.push(window.VP_History.onChange(renderHistory));
    w.removers.push(window.VP_I18n.onLanguageChanged(function () {
      renderHistory();
      renderJob();
    }));
    var B = window.VP_Bridge;
    w.removers.push(B.on('translate.progress', onProgress));
    w.removers.push(B.on('translate.cue', onCues));
    w.removers.push(B.on('translate.done', onDone));
    w.removers.push(B.on('translate.error', onError));
    w.removers.push(B.on('project.autosaved', onAutosaved));
    var handlers = keyHandlers();
    KEY_ACTIONS.forEach(function (action) { w.removers.push(window.VP_Keys.handle(action, handlers[action], OWNER)); });

    S.setCueTotal(project.cues || 0);
    window.VP_CueList.mount(w.left, { total: project.cues || 0, kind: project.kind, pair: project.pair });
    window.VP_Panes.mount(w.panes, { kind: project.kind, pair: project.pair });
    setMode(params.mode || (project.pair === 'la-la' ? 'orberg' : 'translate'));
    renderSave();
    renderCounts();
    renderEngines();
    renderHistory();
    renderJob();
    window.VP_I18n.bind(w.root);
    if ((project.cues || 0) > 0) { window.VP_CueList.select(typeof params.select === 'number' ? params.select : 0); }
  }

  function destroy() {
    if (!w) { return; }
    window.VP_Panes.destroy();
    window.VP_CueList.destroy();
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (w.removers.length) { w.removers.pop()(); }
    if (w.appEl) { w.appEl.classList.remove('vp-app-ws'); }
    if (w.root && w.root.parentNode) { w.root.parentNode.removeChild(w.root); }
    w = null;
  }

  function i18nKeys() {
    var keys = ['workspace.view.showMacrons.on.label', 'workspace.view.showMacrons.off.label', 'workspace.view.showEmoji.on.label', 'workspace.view.showEmoji.off.label',
      'workspace.save.saved.label', 'workspace.save.autosaved.label', 'workspace.save.never.label', 'workspace.save.pending.label', 'workspace.save.pendingOff.label', 'workspace.undo.tooltip', 'workspace.redo.tooltip',
      'workspace.job.progress.label', 'workspace.job.done.label', 'workspace.job.cancelled.label', 'workspace.translate.all.cta', 'workspace.translate.rest.cta',
      'unit.minutes', 'unit.seconds'];
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
    cmd: { review: review, edit: edit, choose: choose, undo: undo, redo: redo, translate: translate, cancel: cancel, refetch: refetch },
    keyActions: function () { return KEY_ACTIONS.slice(); },
    i18nKeys: i18nKeys
  };
}());
