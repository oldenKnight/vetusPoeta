describe('VP_Toast', function () {
  function setup() {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_toast.js']);
    var I = env.window.VP_I18n;
    I.load('en-US', { 'x.saved.label': 'Saved {name}', 'toast.undo.cta': 'Undo', 'toast.dismiss.aria': 'Close notice', 'toast.region.aria': 'Notices' });
    I.load('es-MX', { 'x.saved.label': 'Se guardó {name}', 'toast.undo.cta': 'Deshacer', 'toast.dismiss.aria': 'Cerrar aviso', 'toast.region.aria': 'Avisos' });
    return env;
  }

  it('shows text in a role=status live region and disappears after 6 s', function () {
    var env = setup();
    var T = env.window.VP_Toast;
    T.show({ key: 'x.saved.label', vars: { name: 'a.srt' } });
    var region = env.document.getElementById('vp-toasts');
    eq(region.getAttribute('role'), 'status');
    eq(region.getAttribute('aria-live'), 'polite');
    eq(region.getAttribute('aria-label'), 'Notices');
    eq(region.querySelector('.vp-toast-text').textContent, 'Saved a.srt');
    eq(region.querySelector('.vp-toast-close').getAttribute('aria-label'), 'Close notice');
    eq(T.count(), 1);
    env.clock.tick(5999);
    eq(T.count(), 1);
    env.clock.tick(1);
    eq(T.count(), 0);
    eq(region.childNodes.length, 0);
    eq(env.window.VP_Dom.count(), 0);
    eq(env.window.VP_Timers.count(), 0);
  });

  it('Undo runs the action once and closes the toast; the text follows the language', function () {
    var env = setup();
    var T = env.window.VP_Toast;
    var undone = 0;
    T.undoable('x.saved.label', { name: 'b' }, function () { undone++; });
    var region = env.document.getElementById('vp-toasts');
    env.window.VP_I18n.setLang('es-MX');
    eq(region.querySelector('.vp-toast-text').textContent, 'Se guardó b');
    eq(region.querySelector('.vp-toast-action').textContent, 'Deshacer');
    region.querySelector('.vp-toast-action').click();
    eq(undone, 1);
    eq(T.count(), 0);
    eq(env.window.VP_Timers.count(), 0);
    eq(env.window.VP_Dom.count(), 0);
  });

  it('keeps at most three and can dismiss or clear them all', function () {
    var env = setup();
    var T = env.window.VP_Toast;
    var ids = [];
    for (var i = 0; i < 5; i++) { ids.push(T.show({ text: 'n' + i })); }
    eq(T.count(), 3);
    eq(env.document.getElementById('vp-toasts').firstChild.textContent, 'n2');
    ok(T.dismiss(ids[4]));
    eq(T.dismiss(ids[0]), false);
    T.clearAll();
    eq(T.count(), 0);
    eq(env.window.VP_Timers.count(), 0);
    eq(env.window.VP_Dom.count(), 0);
  });
});
