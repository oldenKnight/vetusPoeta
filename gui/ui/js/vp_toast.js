/* vp_toast.js - short notices at the bottom centre (PREDESIGN 2.3): 6 s, optional action
 * (usually Undo), inside one role="status" aria-live="polite" region. At most 3 at once.
 *
 * VP_Toast.show({key, vars, kind, actionKey, onAction, timeoutMs}) -> id
 *   kind: 'info' | 'success' | 'error'; the text is the i18n key (re-translated on a
 *   language switch) or, for engine fallbacks, opts.text
 * VP_Toast.undoable(key, vars, onUndo) -> id; dismiss(id); clearAll(); count()
 */
(function () {
  'use strict';

  var MAX = 3;
  var DEFAULT_MS = 6000;

  var toasts = [];
  var nextId = 1;
  var region = null;

  function ensureRegion() {
    if (region && region.parentNode) { return region; }
    region = document.getElementById('vp-toasts');
    if (!region) {
      region = window.VP_Dom.el('div', { id: 'vp-toasts', className: 'vp-toasts', role: 'status', 'aria-live': 'polite', 'data-i18n-aria': 'toast.region.aria' });
      document.body.appendChild(region);
      window.VP_I18n.bind(region);
    }
    return region;
  }

  function find(id) {
    for (var i = 0; i < toasts.length; i++) { if (toasts[i].id === id) { return i; } }
    return -1;
  }

  function dismiss(id) {
    var i = find(id);
    if (i < 0) { return false; }
    var t = toasts[i];
    toasts.splice(i, 1);
    window.VP_Dom.offOwner('toast:' + id);
    window.VP_Timers.clear(t.timer);
    if (t.el.parentNode) { t.el.parentNode.removeChild(t.el); }
    return true;
  }

  function show(opts) {
    opts = opts || {};
    var D = window.VP_Dom;
    var id = nextId++;
    var owner = 'toast:' + id;
    var kind = opts.kind || 'info';
    var textAttrs = { className: 'vp-toast-text' };
    if (opts.key) {
      textAttrs['data-i18n'] = opts.key;
      if (opts.vars) { textAttrs['data-i18n-vars'] = opts.vars; }
      textAttrs.text = window.VP_I18n.t(opts.key, opts.vars);
    } else {
      textAttrs.text = String(opts.text || '');
    }
    var children = [D.el('span', textAttrs)];
    if (opts.actionKey && typeof opts.onAction === 'function') {
      var action = D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-toast-action', 'data-i18n': opts.actionKey, text: window.VP_I18n.t(opts.actionKey) });
      D.on(action, 'click', function () {
        dismiss(id);
        opts.onAction();
      }, { owner: owner });
      children.push(action);
    }
    var close = D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon vp-toast-close', 'data-i18n-aria': 'toast.dismiss.aria', 'aria-label': window.VP_I18n.t('toast.dismiss.aria') }, [
      D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' }, [D.el('use', { href: '#vp-i-close' })])
    ]);
    D.on(close, 'click', function () { dismiss(id); }, { owner: owner });
    children.push(close);
    var node = D.el('div', { className: 'vp-toast vp-toast-' + kind, dataset: { toastId: id } }, children);
    ensureRegion().appendChild(node);
    var timer = window.VP_Timers.setTimeout('toast', function () { dismiss(id); }, opts.timeoutMs || DEFAULT_MS);
    toasts.push({ id: id, el: node, timer: timer });
    while (toasts.length > MAX) { dismiss(toasts[0].id); }
    return id;
  }

  function undoable(key, vars, onUndo) {
    return show({ key: key, vars: vars, actionKey: 'toast.undo.cta', onAction: onUndo, kind: 'success' });
  }

  function clearAll() {
    while (toasts.length) { dismiss(toasts[0].id); }
  }

  window.VP_Toast = {
    show: show,
    undoable: undoable,
    dismiss: dismiss,
    clearAll: clearAll,
    count: function () { return toasts.length; }
  };
}());
