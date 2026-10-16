describe('VP_Store', function () {
  function setup() { return load(['vp_store.js']); }

  it('get/set/patch with key and wildcard subscribers, dotted reads', function () {
    var S = setup().window.VP_Store;
    var seen = [];
    var all = [];
    var off = S.subscribe('settings', function (v) { seen.push(v.theme); });
    S.subscribe('*', function (v, key) { all.push(key); });
    S.set('settings', { theme: 'dark', export: { macrons: false } });
    S.patch('settings', { theme: 'light' });
    eq(S.get('settings.theme'), 'light');
    eq(S.get('settings.export.macrons'), false);
    eq(S.get('nothing.here'), undefined);
    deepEq(seen, ['dark', 'light']);
    deepEq(all, ['settings', 'settings']);
    eq(S.subscriberCount(), 2);
    off();
    ok(!S.unsubscribe('settings', function () {}));
    eq(S.subscriberCount(), 1);
  });

  it('a failing subscriber does not stop the others', function () {
    var env = setup();
    var S = env.window.VP_Store;
    var n = 0;
    S.subscribe('x', function () { throw new Error('bad'); });
    S.subscribe('x', function () { n++; });
    S.set('x', 1);
    eq(n, 1);
    eq(env.errors().length, 1);
  });

  it('caps the cue window at 50,000 records, dropping the oldest', function () {
    var S = setup().window.VP_Store;
    eq(S.cueCap(), 50000);
    var batch = [];
    for (var i = 0; i < 50100; i++) {
      batch.push({ index: i, target: 't' });
      if (batch.length === 1000) {
        S.putCues(batch);
        batch = [];
      }
    }
    S.putCues(batch);
    eq(S.cueCount(), 50000);
    eq(S.getCue(0), null);
    eq(S.getCue(99), null);
    eq(S.getCue(100).index, 100);
    eq(S.getCue(50099).index, 50099);
    eq(S.stats().evicted, 100);
    S.putCues([{ index: 50099, target: 'new' }]);
    eq(S.cueCount(), 50000);
    eq(S.getCue(50099).target, 'new');
  });

  it('reset() nulls the large arrays and the project', function () {
    var S = setup().window.VP_Store;
    S.set('project', { name: 'x' });
    S.set('settings', { theme: 'dark' });
    S.putCues([{ index: 0 }, { index: 1 }]);
    S.setCueTotal(2);
    S.reset();
    eq(S.cueCount(), 0);
    eq(S.cueTotal(), 0);
    eq(S.get('project'), null);
    eq(S.get('settings').theme, 'dark');
    eq(S.stats().arraysNull, true);
    S.putCues([{ index: 5 }]);
    eq(S.cueCount(), 1);
  });
});
