describe('VP_Router', function () {
  var FILES = ['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_bridge.js', 'vp_store.js', 'vp_history.js', 'vp_keys.js', 'vp_router.js', 'vp_debug.js'];
  function setup() {
    var env = load(FILES);
    env.window.VP_Debug.enable(true);
    env.root = env.window.VP_Dom.el('main');
    env.document.body.appendChild(env.root);
    return env;
  }
  function cleanScreen(env) {
    var W = env.window;
    var removers = [];
    return {
      mount: function (root) {
        var b = W.VP_Dom.el('button', { className: 'go' });
        root.appendChild(b);
        W.VP_Dom.on(b, 'click', function () {}, { owner: 'clean' });
        W.VP_Dom.delegate(root, '.go', 'keydown', function () {}, { owner: 'clean' });
        W.VP_Timers.setInterval('clean', function () {}, 1000);
        removers.push(W.VP_Store.subscribe('engine', function () {}));
        removers.push(W.VP_I18n.onLanguageChanged(function () {}));
        removers.push(W.VP_Bridge.on('translate.cue', function () {}));
        removers.push(W.VP_Keys.handle('save', function () {}, 'clean'));
      },
      destroy: function () {
        W.VP_Dom.offOwner('clean');
        W.VP_Timers.clearAll('clean');
        while (removers.length) { removers.pop()(); }
      }
    };
  }

  it('mounts, destroys and remounts 50 times with every count back at its baseline', function () {
    var env = setup();
    var R = env.window.VP_Router;
    var mounts = 0;
    var screen = cleanScreen(env);
    var counting = { mount: function (root) { mounts++; screen.mount(root); }, destroy: screen.destroy };
    R.register('start', counting);
    R.register('other', cleanScreen(env));
    var before = R.counts();
    env.root.appendChild(env.window.VP_Dom.el('p', { className: 'vp-loading' }));
    R.start(env.root, 'start');
    eq(env.root.querySelectorAll('.vp-loading').length, 0, 'start() removes the loading text');
    for (var i = 0; i < 50; i++) { R.go(i % 2 ? 'start' : 'other'); }
    R.stop();
    deepEq(R.counts(), before);
    deepEq(env.window.VP_Debug.failures(), []);
    eq(mounts, 26);
    eq(env.root.childNodes.length, 0);
    eq(env.listenerCount(), 0);
    eq(env.clock.pending(), 0);
  });

  it('reports a screen that leaks a listener, a timer or a subscription', function () {
    var env = setup();
    var W = env.window;
    var R = W.VP_Router;
    R.register('start', cleanScreen(env));
    R.register('leaky', {
      mount: function (root) {
        W.VP_Dom.on(root, 'click', function () {});
        W.VP_Timers.setTimeout('leaky', function () {}, 99999);
        W.VP_Store.subscribe('engine', function () {});
      },
      destroy: function () {}
    });
    R.start(env.root, 'leaky');
    R.go('start');
    var f = W.VP_Debug.failures();
    eq(f.length, 1);
    eq(f[0].code, 'router.leak');
    eq(f[0].detail.screen, 'leaky');
    deepEq(f[0].detail.diff.listeners, { before: 0, after: 1 });
    deepEq(f[0].detail.diff.timers, { before: 0, after: 1 });
    deepEq(f[0].detail.diff.store, { before: 0, after: 1 });
    eq(env.errors().length, 1, 'console.error in dev mode');
  });

  it('skips leak checks when debug is off; survives a throwing screen; falls back on unknown names', function () {
    var env = setup();
    var W = env.window;
    var R = W.VP_Router;
    W.VP_Debug.enable(false);
    R.register('start', cleanScreen(env));
    R.register('leaky', { mount: function (root) { W.VP_Dom.on(root, 'click', function () {}); }, destroy: function () {} });
    R.register('broken', { mount: function () { throw new Error('mount failed'); }, destroy: function () { throw new Error('destroy failed'); } });
    R.start(env.root, 'leaky');
    R.go('broken');
    eq(R.current(), 'broken');
    R.go('nowhere');
    eq(R.current(), 'start');
    eq(env.root.getAttribute('data-screen'), 'start');
    var codes = W.VP_Debug.failures().map(function (x) { return x.code; });
    deepEq(codes, ['router.mount', 'router.destroy']);
    throws(function () { R.register('bad', {}); }, /mount\(\) and destroy\(\)/);
    deepEq(R.names().sort(), ['broken', 'leaky', 'start']);
  });
});
