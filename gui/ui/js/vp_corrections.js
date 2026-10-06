/* vp_corrections.js - the Corrections tab: the correction memory (DESIGN 13; PREDESIGN 4.2).
 *
 * corrections.list gives {id, key, target, scope, count}: the normalised source phrase (or
 * whole cue), the Latin or Greek text it becomes, the scope ("phrase" | "cue" | "project"),
 * and how many times it was applied. Each row has Remove -> corrections.remove with an Undo
 * toast; the engine has no corrections.add, so Undo re-creates the entry by sending cue.set
 * with remember on the cue it came from when that is known, else it says it cannot.
 *
 * VP_Corrections.mount(el) / destroy(); reload() -> Promise; remove(id) -> Promise;
 * corrections(); stats()
 */
(function () {
  'use strict';

  var OWNER = 'corrections';
  var s = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }
  function P() { return window.Promise; }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function langs() {
    var p = window.VP_Store.get('project') || {};
    var parts = String(p.pair || 'en-la').split('-');
    return { src: parts[0], dst: parts[1] || parts[0] };
  }

  function scopeKey(scope) {
    var k = 'corrections.scope.' + String(scope || 'phrase') + '.label';
    return window.VP_I18n.has(k) ? k : 'corrections.scope.phrase.label';
  }

  function render() {
    if (!s) { return; }
    var D = window.VP_Dom;
    var L = langs();
    D.clear(s.list);
    if (!s.loaded) {
      s.list.appendChild(i18nEl('p', 'vp-hint', 'corrections.loading.label'));
      return;
    }
    if (!s.items.length) {
      s.list.appendChild(i18nEl('p', 'vp-hint', 'corrections.panel.empty'));
      return;
    }
    s.items.forEach(function (c) {
      s.list.appendChild(el('li', { className: 'vp-corr', dataset: { corrId: c.id } }, [
        el('div', { className: 'vp-corr-pair' }, [
          el('span', { className: 'vp-corr-from vp-text', lang: L.src, text: c.key || c.from || '' }),
          el('span', { className: 'vp-corr-arrow', 'aria-hidden': 'true', text: '→' }),
          el('span', { className: 'vp-corr-to vp-text', lang: L.dst, text: c.target || c.to || '' })
        ]),
        el('div', { className: 'vp-corr-meta' }, [
          i18nEl('span', 'vp-corr-scope', scopeKey(c.scope || c.kind)),
          el('span', { className: 'vp-corr-count', text: T('corrections.count', { n: c.count || 0 }) }),
          el('span', { className: 'vp-spacer' }),
          i18nEl('button', 'vp-btn vp-btn-tertiary', 'corrections.remove.cta', null, { type: 'button', dataset: { corrAction: 'remove' }, 'aria-label': T('corrections.remove.aria', { from: c.key || '' }) })
        ])
      ]));
    });
  }

  function accept(r) {
    if (!s) { return []; }
    s.items = (r && r.corrections) || [];
    s.loaded = true;
    render();
    return s.items;
  }

  function reload() {
    if (!s) { return P().resolve([]); }
    var gen = s.gen;
    return window.VP_Bridge.call('corrections.list', {}).then(function (r) {
      return s && s.gen === gen ? accept(r) : [];
    }, function (err) {
      if (s && s.gen === gen) {
        accept({ corrections: [] });
        showError(err);
      }
      return [];
    });
  }

  function find(id) {
    for (var i = 0; i < s.items.length; i++) { if (s.items[i].id === id) { return s.items[i]; } }
    return null;
  }

  // Undo of a removal: the engine has no corrections.add, so the entry is re-created through
  // cue.set {remember} on a cue whose source matches the key; if none is loaded, a notice.
  function restore(c) {
    var total = window.VP_Store.cueTotal();
    var key = String(c.key || '').toLowerCase();
    for (var i = 0; i < total; i++) {
      var cue = window.VP_Store.getCue(i);
      if (cue && String(cue.source || '').toLowerCase() === key) {
        return window.VP_Workspace.cmd.edit(i, c.target, c.scope === 'cue' ? 'cue' : 'phrase').then(function () { return reload(); });
      }
    }
    window.VP_Toast.show({ key: 'corrections.restore.none.label', kind: 'error' });
    return P().resolve(null);
  }

  function remove(id) {
    if (!s) { return P().resolve(false); }
    var c = find(id);
    if (!c) { return P().resolve(false); }
    return window.VP_Bridge.call('corrections.remove', { id: id }).then(function (r) {
      accept(r);
      window.VP_Toast.undoable('corrections.removed.label', null, function () { restore(c).then(null, showError); });
      return true;
    }, function (err) {
      showError(err);
      return false;
    });
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-corr-action');
    if (a === 'remove') {
      var li = window.VP_Dom.closest(btn, '.vp-corr', s.root);
      if (li) { remove(li.getAttribute('data-corr-id')); }
    } else if (a === 'reload') {
      reload();
    }
  }

  function mount(root) {
    if (s) { destroy(); }
    s = { gen: (mount.gen = (mount.gen || 0) + 1), items: [], loaded: false, removers: [] };
    s.list = el('ul', { className: 'vp-corr-list' });
    s.root = el('div', { className: 'vp-panel vp-panel-corrections' }, [
      i18nEl('h2', 'vp-panel-title', 'corrections.panel.title'),
      i18nEl('p', 'vp-hint', 'corrections.panel.hint'),
      s.list,
      el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-tertiary', 'corrections.reload.cta', null, { type: 'button', dataset: { corrAction: 'reload' } })])
    ]);
    root.appendChild(s.root);
    window.VP_Dom.delegate(s.root, '[data-corr-action]', 'click', onClick, { owner: OWNER });
    s.removers.push(window.VP_I18n.onLanguageChanged(render));
    // A new correction (cue.set with remember) shows up on the next cue change.
    s.removers.push(window.VP_Store.subscribe('cues', function () {
      if (!s || s.timer !== null) { return; }
      s.timer = window.VP_Timers.setTimeout(OWNER, function () {
        if (s) { s.timer = null; }
        reload();
      }, 400);
    }));
    s.timer = null;
    render();
    reload();
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
    return ['corrections.count', 'corrections.removed.label', 'corrections.remove.aria', 'corrections.scope.phrase.label', 'corrections.scope.cue.label', 'corrections.scope.project.label', 'corrections.restore.none.label'];
  }

  window.VP_Corrections = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    reload: reload,
    remove: remove,
    corrections: function () { return s ? s.items.slice() : []; },
    stats: function () { return s ? { items: s.items.length, loaded: s.loaded } : null; },
    i18nKeys: i18nKeys
  };
}());
