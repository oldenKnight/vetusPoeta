describe('VP_MockEngine', function () {
  function setup() {
    var env = load(['vp_dom.js', 'vp_bridge.js', 'vp_mock_engine.js']);
    env.window.VP_Bridge.init({ mock: true });
    env.call = function (cmd, params) {
      var out = { result: null, error: null };
      env.window.VP_Bridge.call(cmd, params).then(function (r) { out.result = r; }, function (e) { out.error = e; });
      env.clock.tick(50);
      return out;
    };
    return env;
  }

  it('generate(n) is deterministic for a seed and produces CueView fields', function () {
    var M = setup().window.VP_MockEngine;
    var a = M.generate(30, { seed: 3, translated: true });
    var b = M.generate(30, { seed: 3, translated: true });
    var c = M.generate(30, { seed: 4, translated: true });
    deepEq(a, b);
    ok(JSON.stringify(a) !== JSON.stringify(c));
    eq(a.length, 30);
    var cue = a[0];
    ['index', 'idRaw', 'timingRaw', 'start', 'end', 'durationMs', 'source', 'target', 'state', 'confidence', 'score', 'cps', 'lines', 'flags'].forEach(function (k) {
      ok(Object.prototype.hasOwnProperty.call(cue, k), 'CueView.' + k);
    });
    ok(/^\d\d:\d\d:\d\d,\d\d\d --> \d\d:\d\d:\d\d,\d\d\d$/.test(cue.timingRaw), cue.timingRaw);
    ok(['ok', 'check', 'fix'].indexOf(cue.confidence) >= 0);
    eq(cue.state, 'translated');
    ok(a[1].start > a[0].end, 'cues do not overlap');
    eq(M.generate(50000).length, 50000);
  });

  it('new project, pages of at most 200 cues, Greek pair targets', function () {
    var env = setup();
    var p = env.call('project.new', { kind: 'subs', pair: 'en-grc', count: 250 }).result.project;
    eq(p.cues, 250);
    eq(p.pair, 'en-grc');
    var page = env.call('cue.page', { from: 200, count: 100 }).result;
    eq(page.total, 250);
    eq(page.cues.length, 50);
    eq(env.call('cue.page', { from: 0, count: 201 }).error.code, 'bad_params');
    eq(env.call('project.new', { kind: 'subs', pair: 'xx-yy' }).error.code, 'bad_params');
    env.call('translate.start', { indices: [0, 1], engines: { rules: true }, fidelity: 2 });
    env.clock.tick(1000);
    var cue = env.call('cue.get', { index: 0 }).result;
    ok(/[\u0370-\u03FF\u1F00-\u1FFF]/.test(cue.cue.target), 'Greek text: ' + cue.cue.target);
    eq(cue.alternatives.length, 3);
    eq(cue.checks.length, 9);
  });

  it('runs a translate job with progress and done events, busy while running, cancel works', function () {
    var env = setup();
    var B = env.window.VP_Bridge;
    var progress = [];
    var done = [];
    B.on('translate.progress', function (e) { progress.push(e.done); });
    B.on('translate.done', function (e) { done.push(e.stats); });
    env.call('project.new', { kind: 'subs', pair: 'en-la', count: 100 });
    var job = env.call('translate.start', { engines: { rules: true }, fidelity: 2 }).result.jobId;
    ok(job);
    eq(env.call('translate.start', {}).error.code, 'busy');
    env.clock.tick(2000);
    eq(done.length, 1);
    eq(done[0].translated, 100);
    eq(progress[progress.length - 1], 100);
    eq(done[0].ok + done[0].check + done[0].fix, 100);
    var page = env.call('cue.page', { from: 0, count: 100 }).result;
    ok(page.cues.every(function (c) { return c.state === 'translated' && c.target; }));
    env.call('translate.start', { engines: { rules: true }, fidelity: 2 });
    env.call('translate.cancel', {});
    env.clock.tick(2000);
    eq(done.length, 2);
    eq(done[1].cancelled, true);
  });

  it('edits, undo/redo, review, export preview without macrons, settings and online check', function () {
    var env = setup();
    env.call('project.open', { path: 'C:\\x\\Fabula.vpoeta' });
    var before = env.call('cue.get', { index: 0 }).result.cue.target;
    eq(env.call('cue.set', { index: 0, text: 'Salvē.' }).result.cue.state, 'edited');
    var u = env.call('history.undo', {}).result;
    deepEq(u.changedIndices, [0]);
    eq(env.call('cue.get', { index: 0 }).result.cue.target, before);
    eq(env.call('history.redo', {}).result.canRedo, false);
    eq(env.call('cue.get', { index: 0 }).result.cue.target, 'Salvē.');
    eq(env.call('cue.review', { indices: [0, 1], reviewed: true }).result.count, 2);
    eq(env.call('export.preview', { indices: [0], macrons: false }).result.cues[0].lines[0], 'Salve.');
    eq(env.call('export.write', { path: 'exists.srt', format: 'srt' }).error.code, 'io');
    eq(env.call('online.test', {}).error.code, 'online_disabled');
    eq(env.call('settings.set', { patch: { engines: { online: true } } }).result.engines.online, true);
    eq(env.call('online.test', {}).result.ok, true);
    eq(env.call('settings.set', { patch: { textScale: 300 } }).error.code, 'bad_params');
    eq(env.call('project.open', { path: 'damaged.vpoeta' }).error.code, 'project_corrupt');
    eq(env.call('word.inspect', { text: 'rosam', lang: 'la' }).result.analyses[0].lemma.glossEs, 'rosa');
    eq(env.call('word.inspect', { text: 'xyz', lang: 'la' }).result.analyses.length, 0);
    var withName = env.call('cue.page', { from: 0, count: 40 }).result.cues.filter(function (c) { return c.source.indexOf('Marcus') >= 0; }).map(function (c) { return c.index; });
    deepEq(env.call('names.set', { name: 'Marcus', policy: 'keep' }).result.affectedCues, withName);
    eq(env.call('names.set', { name: 'Iulia', policy: 'decline' }).result.affectedCues.length, 0);
    ok(env.call('words.list', {}).result.words.length > 5);
    eq(env.call('model.status', {}).result.available, false);
  });

  it('its own sample sentences carry macrons and polytonic Greek', function () {
    var s = setup().window.VP_MockEngine.sentences();
    ok(s.length >= 12);
    ok(s.some(function (x) { return /[āēīōū]/.test(x.la); }));
    ok(s.every(function (x) { return x.en && x.es && x.la && x.grc; }));
  });
});
