describe('VP_Bridge', function () {
  var FILES = ['vp_dom.js', 'vp_bridge.js', 'vp_mock_engine.js'];
  function setup(opts) { return load(FILES, opts); }

  it('without a transport every call fails with no_engine', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    eq(B.init({}), 'none');
    var err = null;
    B.call('engine.hello').then(null, function (e) { err = e; });
    env.clock.flush();
    eq(err.code, 'no_engine');
    eq(err.cmd, 'engine.hello');
  });

  it('talks to the mock: request ids, results, error objects with code and hint', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    var M = env.window.VP_MockEngine;
    eq(B.init({ mock: true }), 'mock');
    var hello = null;
    var bad = null;
    var unknown = null;
    B.call('engine.hello').then(function (r) { hello = r; });
    M.options.failNext = { cmd: 'settings.get', code: 'io', message: 'disk', hint: 'Check the disk.' };
    B.call('settings.get').then(null, function (e) { bad = e; });
    B.call('no.such.command').then(null, function (e) { unknown = e; });
    eq(B.stats().pending, 3);
    env.clock.tick(M.options.latencyMs);
    eq(hello.version, '0.0.0-mock');
    eq(hello.lexicons[0].lang, 'la');
    eq(bad.code, 'io');
    eq(bad.hint, 'Check the disk.');
    eq(unknown.code, 'not_found');
    eq(B.stats().pending, 0);
    eq(B.stats().sent, 3);
  });

  it('times out a silent command with code "timeout" and ignores the late answer', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    var M = env.window.VP_MockEngine;
    B.init({ mock: true });
    M.options.latencyMs = 'never';
    var err = null;
    var okv = null;
    B.call('engine.ping').then(function (r) { okv = r; }, function (e) { err = e; });
    env.clock.tick(1999);
    eq(err, null);
    env.clock.tick(1);
    eq(err.code, 'timeout');
    eq(B.stats().pending, 0);
    eq(B.stats().timeouts, 1);
    B.inject({ id: 1, ok: true, result: {} });
    env.clock.flush();
    eq(okv, null);
    eq(env.clock.pending(), 0);
  });

  it('uses window.chrome.webview when present (JSON strings both ways)', function () {
    var env = setup({ webview: true });
    var B = env.window.VP_Bridge;
    eq(B.init({ mock: true }), 'webview');
    var got = null;
    B.call('settings.get', { a: 1 }).then(function (r) { got = r; });
    eq(env.webviewSent.length, 1);
    var sent = JSON.parse(env.webviewSent[0]);
    eq(sent.cmd, 'settings.get');
    deepEq(sent.params, { a: 1 });
    env.webviewReply({ id: sent.id, ok: true, result: { theme: 'dark' } });
    env.clock.flush();
    eq(got.theme, 'dark');
    eq(env.window.VP_Dom.count(), 1);
    B.disconnect();
    eq(env.window.VP_Dom.count(), 0);
    eq(env.listenerCount(), 0);
  });

  it('delivers events in animation-frame batches; on() returns a remover', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    B.init({ mock: true });
    var got = [];
    var off = B.on('power.status', function (e) { got.push(e.onBattery); });
    B.inject(JSON.stringify({ event: 'power.status', onBattery: true }));
    eq(got.length, 0);
    env.clock.tick(16);
    deepEq(got, [true]);
    eq(B.handlerCount(), 1);
    off();
    eq(B.handlerCount(), 0);
    B.inject({ event: 'power.status', onBattery: false });
    env.clock.tick(200);
    deepEq(got, [true]);
  });

  it('coalesces a 1,000 events/s translate.cue burst into at most 10 UI updates per second', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    B.init({ mock: true });
    var cueCalls = 0;
    var cues = 0;
    var progressCalls = 0;
    var lastDone = 0;
    var finished = 0;
    B.on('translate.cue', function (e) { cueCalls++; cues += e.cues.length; });
    B.on('translate.progress', function (e) { progressCalls++; lastDone = e.done; });
    B.on('translate.done', function () { finished++; });
    for (var i = 0; i < 1000; i++) {
      B.inject({ event: 'translate.cue', jobId: 'j1', cue: { index: i } });
      B.inject({ event: 'translate.progress', jobId: 'j1', done: i + 1, total: 1000 });
      env.clock.tick(1);
    }
    B.inject({ event: 'translate.done', jobId: 'j1', stats: {} });
    env.clock.tick(300);
    eq(cues, 1000);
    eq(lastDone, 1000);
    eq(finished, 1);
    ok(cueCalls <= 11, 'cue updates in 1 s: ' + cueCalls);
    ok(progressCalls <= 11, 'progress updates in 1 s: ' + progressCalls);
    ok(B.stats().flushes <= 12, 'flushes ' + B.stats().flushes);
  });

  it('a real mock job at 1,000 cues/s arrives complete and throttled', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    var M = env.window.VP_MockEngine;
    M.options.cuesPerSecond = 1000;
    M.options.batch = 1;
    B.init({ mock: true });
    var cues = {};
    var calls = 0;
    var done = null;
    B.on('translate.cue', function (e) { calls++; e.cues.forEach(function (c) { cues[c.index] = c.state; }); });
    B.on('translate.done', function (e) { done = e.stats; });
    B.call('project.new', { kind: 'subs', pair: 'en-la', count: 500 });
    env.clock.tick(20);
    B.call('translate.start', { engines: { rules: true, model: false, online: false }, fidelity: 2 });
    env.clock.tick(2000);
    eq(Object.keys(cues).length, 500);
    eq(done.translated, 500);
    ok(calls <= 8, 'UI updates for a 0.5 s job: ' + calls);
  });

  it('engine.restarted fails what is in flight; a failing handler does not stop the others', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    var M = env.window.VP_MockEngine;
    B.init({ mock: true });
    M.options.latencyMs = 'never';
    var err = null;
    var n = 0;
    B.call('engine.hello').then(null, function (e) { err = e; });
    B.on('engine.restarted', function () { throw new Error('bad handler'); });
    B.on('engine.restarted', function () { n++; });
    B.inject({ event: 'engine.restarted', recovered: true });
    env.clock.tick(20);
    eq(err.code, 'internal');
    eq(n, 1);
    eq(env.errors().length, 1);
  });
});
