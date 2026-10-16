describe('VP_Debug', function () {
  it('is off by default: stats() is null', function () {
    var env = load(['vp_debug.js']);
    eq(env.window.VP_Debug.enabled(), false);
    eq(env.window.VP_Debug.stats(), null);
  });

  it('turns on with ?debug=1 or localStorage vp.debug=1', function () {
    eq(load(['vp_debug.js'], { search: '?mock=1&debug=1' }).window.VP_Debug.enabled(), true);
    var env = load(['vp_debug.js']);
    env.window.localStorage.setItem('vp.debug', '1');
    eq(env.window.VP_Debug.enabled(), true);
  });

  it('stats() reports listeners, timers, DOM nodes, cue rows and caches', function () {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_store.js', 'vp_history.js', 'vp_debug.js'], { search: '?debug=1' });
    var W = env.window;
    W.VP_Dom.on(env.document, 'click', function () {});
    W.VP_Timers.setTimeout('x', function () {}, 10);
    env.document.body.appendChild(W.VP_Dom.el('div', { className: 'vp-cue-row' }));
    W.VP_Store.putCues([{ index: 1 }, { index: 2 }]);
    var off = W.VP_Debug.registerCache('inspector', function () { return 7; });
    var s = W.VP_Debug.stats();
    eq(s.listeners, 1);
    eq(s.timers, 1);
    eq(s.cueRows, 1);
    eq(s.domNodes, 4);
    eq(s.caches.cues, 2);
    eq(s.caches.inspector, 7);
    eq(s.caches.history, 0);
    off();
    eq(W.VP_Debug.stats().caches.inspector, undefined);
  });

  it('records failures (capped) and logs them as console errors', function () {
    var env = load(['vp_debug.js']);
    var Dbg = env.window.VP_Debug;
    eq(Dbg.assert(true, 'fine'), true);
    eq(Dbg.assert(false, 'bad', { a: 1 }), false);
    for (var i = 0; i < 200; i++) { Dbg.fail('more'); }
    eq(Dbg.failures().length, 100);
    eq(Dbg.failures()[0].code, 'bad');
    eq(env.errors().length, 201);
    Dbg.clearFailures();
    eq(Dbg.failures().length, 0);
  });
});
