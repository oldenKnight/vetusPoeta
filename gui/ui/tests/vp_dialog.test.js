describe('VP_Dialog', function () {
  function setup() {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_keys.js', 'vp_dialog.js']);
    env.window.VP_I18n.load('en-US', {
      'd.title': 'Save changes', 'd.text': 'Keep {n} edits', 'dialog.ok.cta': 'OK', 'dialog.cancel.cta': 'Cancel',
      'dialog.close.aria': 'Close', 'd.third': 'Later'
    });
    var opener = env.window.VP_Dom.el('button', { id: 'opener' });
    env.document.body.appendChild(opener);
    opener.focus();
    env.opener = opener;
    return env;
  }

  it('is a labelled modal dialog that takes focus and gives it back', function () {
    var env = setup();
    var Dlg = env.window.VP_Dialog;
    var closed = [];
    var h = Dlg.open({ titleKey: 'd.title', textKey: 'd.text', textVars: { n: 3 }, onClose: function (v) { closed.push(v); } });
    var box = h.el;
    eq(box.getAttribute('role'), 'dialog');
    eq(box.getAttribute('aria-modal'), 'true');
    var title = env.document.getElementById(box.getAttribute('aria-labelledby'));
    eq(title.textContent, 'Save changes');
    eq(box.querySelector('p').textContent, 'Keep 3 edits');
    eq(env.document.activeElement.getAttribute('aria-label'), 'Close');
    ok(env.window.VP_Keys.isSuspended());
    env.key(env.document.activeElement, 'Escape');
    deepEq(closed, [null]);
    eq(Dlg.count(), 0);
    eq(env.document.activeElement, env.opener);
    eq(env.window.VP_Keys.isSuspended(), false);
    eq(env.window.VP_Dom.count(), 0);
    eq(env.listenerCount(), 0);
  });

  it('traps Tab and Shift+Tab inside the dialog', function () {
    var env = setup();
    var h = env.window.VP_Dialog.open({ titleKey: 'd.title', actions: [{ labelKey: 'dialog.cancel.cta', value: 0 }, { labelKey: 'd.third', value: 1 }, { labelKey: 'dialog.ok.cta', value: 2, kind: 'primary' }] });
    var list = env.window.VP_Dom.focusables(h.el);
    eq(list.length, 4);
    list[3].focus();
    eq(env.key(list[3], 'Tab'), false);
    eq(env.document.activeElement, list[0]);
    eq(env.key(list[0], 'Tab', { shift: true }), false);
    eq(env.document.activeElement, list[3]);
    list[1].focus();
    eq(env.key(list[1], 'Tab'), true, 'Tab inside the dialog is left to the browser');
    env.opener.focus();
    ok(h.el.contains(env.document.activeElement), 'focus pulled back inside');
    h.close('x');
    eq(env.window.VP_Dom.count(), 0);
  });

  it('action buttons close with their value; onClick returning false keeps it open', function () {
    var env = setup();
    var got = [];
    var keep = true;
    var h = env.window.VP_Dialog.open({
      titleKey: 'd.title',
      actions: [{ labelKey: 'd.third', value: 'later', onClick: function () { return keep ? false : undefined; } }, { labelKey: 'dialog.ok.cta', value: 'ok' }],
      onClose: function (v) { got.push(v); }
    });
    var buttons = h.el.querySelectorAll('[data-dialog-action]');
    buttons[1].click();
    eq(got.length, 0);
    keep = false;
    buttons[1].click();
    deepEq(got, ['later']);
  });

  it('confirm() resolves true or false; a non-dismissible dialog ignores Esc', function () {
    var env = setup();
    var D = env.window.VP_Dialog;
    var answers = [];
    D.confirm({ titleKey: 'd.title' }).then(function (v) { answers.push(v); });
    eq(env.document.activeElement.textContent, 'OK');
    env.document.activeElement.click();
    D.confirm({ titleKey: 'd.title' }).then(function (v) { answers.push(v); });
    env.key(env.document.activeElement, 'Escape');
    env.clock.flush();
    deepEq(answers, [true, false]);
    var h = D.open({ titleKey: 'd.title', dismissible: false });
    env.key(env.document.activeElement, 'Escape');
    eq(D.count(), 1);
    eq(h.el.querySelector('.vp-dialog-close'), null);
    D.closeAll();
    eq(D.count(), 0);
    eq(env.window.VP_Dom.count(), 0);
  });
});
