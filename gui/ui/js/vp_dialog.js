/* vp_dialog.js - modal dialogs: role="dialog", aria-modal, labelled by the title, focus trap,
 * Esc closes (when dismissible), focus returns to where it was, global shortcuts suspended.
 *
 * VP_Dialog.open({titleKey, textKey, textVars, body, actions, onClose, initialFocus,
 *                 dismissible, className}) -> handle {id, el, close(value)}
 *   actions: [{labelKey, value, kind: 'primary'|'secondary'|'tertiary', onClick(handle)}]
 *   (onClick returning false keeps the dialog open)
 * VP_Dialog.confirm({titleKey, textKey, textVars, confirmKey, cancelKey}) -> Promise(bool)
 * VP_Dialog.count(), top(), closeAll()
 */
(function () {
  'use strict';

  var stack = [];
  var nextId = 1;

  function focusFirst(box, selector) {
    var target = selector ? box.querySelector(selector) : null;
    if (!target) {
      var list = window.VP_Dom.focusables(box);
      target = list.length ? list[0] : box;
    }
    target.focus();
  }

  function trapTab(box, e) {
    var list = window.VP_Dom.focusables(box);
    if (!list.length) {
      e.preventDefault();
      box.focus();
      return;
    }
    var first = list[0];
    var last = list[list.length - 1];
    var active = document.activeElement;
    var inside = box.contains(active);
    if (e.shiftKey && (active === first || active === box || !inside)) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (active === last || !inside)) {
      e.preventDefault();
      first.focus();
    }
  }

  function close(handle, value) {
    var i = stack.indexOf(handle);
    if (i < 0) { return false; }
    stack.splice(i, 1);
    window.VP_Dom.offOwner(handle.owner);
    if (handle.backdrop.parentNode) { handle.backdrop.parentNode.removeChild(handle.backdrop); }
    window.VP_Keys.resume();
    var back = handle.returnFocus;
    if (back && back.isConnected !== false && typeof back.focus === 'function' && document.body.contains(back)) { back.focus(); }
    if (typeof handle.onClose === 'function') { handle.onClose(value); }
    return true;
  }

  function open(opts) {
    opts = opts || {};
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var id = nextId++;
    var owner = 'dialog:' + id;
    var titleId = 'vp-dialog-title-' + id;
    var dismissible = opts.dismissible !== false;

    var header = [D.el('h2', { id: titleId, className: 'vp-dialog-title', 'data-i18n': opts.titleKey || null, text: opts.titleKey ? T.t(opts.titleKey) : (opts.title || '') })];
    if (dismissible) {
      header.push(D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon vp-dialog-close', 'data-i18n-aria': 'dialog.close.aria', 'aria-label': T.t('dialog.close.aria'), dataset: { dialogAction: 'close' } }, [
        D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' }, [D.el('use', { href: '#vp-i-close' })])
      ]));
    }
    var bodyKids = [];
    if (opts.textKey) { bodyKids.push(D.el('p', { 'data-i18n': opts.textKey, 'data-i18n-vars': opts.textVars || null, text: T.t(opts.textKey, opts.textVars) })); }
    if (opts.body) { bodyKids.push(opts.body); }
    var actions = opts.actions || [{ labelKey: 'dialog.ok.cta', value: true, kind: 'primary' }];
    var buttons = actions.map(function (a, i) {
      return D.el('button', { type: 'button', className: 'vp-btn vp-btn-' + (a.kind || 'secondary'), 'data-i18n': a.labelKey, text: T.t(a.labelKey), dataset: { dialogAction: String(i) } });
    });

    var box = D.el('div', { className: 'vp-dialog' + (opts.className ? ' ' + opts.className : ''), role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': titleId, tabIndex: -1 }, [
      D.el('div', { className: 'vp-dialog-header' }, header),
      D.el('div', { className: 'vp-dialog-body' }, bodyKids),
      D.el('div', { className: 'vp-dialog-actions' }, buttons)
    ]);
    var backdrop = D.el('div', { className: 'vp-dialog-backdrop' }, [box]);

    var handle = {
      id: id, el: box, backdrop: backdrop, owner: owner, onClose: opts.onClose,
      returnFocus: document.activeElement,
      close: function (value) { return close(handle, value); }
    };

    D.on(box, 'keydown', function (e) {
      if (stack[stack.length - 1] !== handle) { return; }
      if (e.key === 'Escape' || e.key === 'Esc') {
        if (dismissible) {
          e.preventDefault();
          e.stopPropagation();
          close(handle, null);
        }
      } else if (e.key === 'Tab') {
        trapTab(box, e);
      }
    }, { owner: owner });
    D.on(document, 'focusin', function (e) {
      if (stack[stack.length - 1] !== handle) { return; }
      if (!box.contains(e.target)) { focusFirst(box, null); }
    }, { owner: owner });
    D.delegate(box, '[data-dialog-action]', 'click', function (e, btn) {
      var which = btn.getAttribute('data-dialog-action');
      if (which === 'close') {
        close(handle, null);
        return;
      }
      var a = actions[Number(which)];
      if (a && typeof a.onClick === 'function' && a.onClick(handle) === false) { return; }
      close(handle, a ? a.value : null);
    }, { owner: owner });

    stack.push(handle);
    window.VP_Keys.suspend();
    document.body.appendChild(backdrop);
    focusFirst(box, opts.initialFocus);
    return handle;
  }

  function confirm(opts) {
    opts = opts || {};
    return new window.Promise(function (resolve) {
      open({
        titleKey: opts.titleKey,
        textKey: opts.textKey,
        textVars: opts.textVars,
        actions: [
          { labelKey: opts.cancelKey || 'dialog.cancel.cta', value: false, kind: 'secondary' },
          { labelKey: opts.confirmKey || 'dialog.ok.cta', value: true, kind: 'primary' }
        ],
        initialFocus: '[data-dialog-action="1"]',
        onClose: function (v) { resolve(v === true); }
      });
    });
  }

  function closeAll() {
    while (stack.length) { close(stack[stack.length - 1], null); }
  }

  window.VP_Dialog = {
    open: open,
    confirm: confirm,
    closeAll: closeAll,
    count: function () { return stack.length; },
    top: function () { return stack.length ? stack[stack.length - 1] : null; }
  };
}());
