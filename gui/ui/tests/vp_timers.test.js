describe('VP_Timers', function () {
  function setup() { return load(['vp_timers.js']); }

  it('setTimeout fires once and stops counting', function () {
    var env = setup();
    var T = env.window.VP_Timers;
    var n = 0;
    T.setTimeout('screen', function () { n++; }, 100);
    eq(T.count(), 1);
    env.clock.tick(99);
    eq(n, 0);
    env.clock.tick(1);
    eq(n, 1);
    eq(T.count(), 0);
    eq(env.clock.pending(), 0);
  });

  it('setInterval repeats until cleared; clear() cancels', function () {
    var env = setup();
    var T = env.window.VP_Timers;
    var n = 0;
    var id = T.setInterval('screen', function () { n++; }, 10);
    env.clock.tick(35);
    eq(n, 3);
    ok(T.clear(id));
    env.clock.tick(100);
    eq(n, 3);
    eq(T.clear(id), false);
    eq(env.clock.pending(), 0);
  });

  it('clearAll(owner) clears only that owner, count(owner) and owners() report', function () {
    var env = setup();
    var T = env.window.VP_Timers;
    var fired = [];
    T.setTimeout('a', function () { fired.push('a1'); }, 10);
    T.setInterval('a', function () { fired.push('a2'); }, 10);
    T.raf('a', function () { fired.push('a3'); });
    T.setTimeout('b', function () { fired.push('b'); }, 10);
    deepEq(T.owners(), { a: 3, b: 1 });
    eq(T.count('a'), 3);
    eq(T.clearAll('a'), 3);
    eq(T.count(), 1);
    env.clock.tick(50);
    deepEq(fired, ['b']);
    eq(env.clock.pending(), 0);
  });

  it('requires an owner and a function', function () {
    var T = setup().window.VP_Timers;
    throws(function () { T.setTimeout('', function () {}, 1); }, /owner/);
    throws(function () { T.setTimeout('x', null, 1); }, /callback/);
  });
});
