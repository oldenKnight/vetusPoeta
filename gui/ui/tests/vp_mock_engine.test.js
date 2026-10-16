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

  it('B8 real shapes: hello (engine, engineKind, pairs, pairsUnavailable, modes, model.rerankEnabled, online, samples)', function () {
    var env = setup();
    var M = env.window.VP_MockEngine;
    var h = env.call('engine.hello').result;
    eq(h.engineKind, 'rules');
    deepEq(h.pairs, ['en-la', 'es-la', 'la-en', 'la-es', 'en-grc', 'grc-en', 'la-la']);
    deepEq(h.pairsUnavailable.map(function (u) { return u.pair + ':' + u.code; }), ['es-grc:bad_params', 'grc-es:bad_params']);
    eq(h.pairsUnavailable[0].hint, 'This language pair is not available yet.');
    deepEq(h.modes, ['R', 'O']);
    eq(h.model.rerankEnabled, false);
    deepEq(h.online, { allowed: false, mock: true, mockCalls: 0 });
    deepEq(h.samples.map(function (x) { return x.lang; }), ['en', 'es', 'la', 'grc']);
    eq(h.samples[2].path, 'mock://data/samples/sample.la.srt');
    M.options.pairs = ['la-en'];
    h = env.call('engine.hello').result;
    deepEq(h.pairs, ['la-en']);
    eq(h.pairsUnavailable.length, 8);
    M.options.pairs = null;
    M.options.noLexicon = true;
    h = env.call('engine.hello').result;
    deepEq(h.pairs, ['en-grc', 'grc-en']);
    eq(h.pairsUnavailable[0].code, 'lexicon_missing');
    M.options.noLexicon = false;
  });

  it('B8 translate.start: an unavailable pair is refused with its hint; a missing model / online check gives warnings and translate.warning events; jobId is a number', function () {
    var env = setup();
    var W = env.window;
    var events = [];
    W.VP_Bridge.on('translate.warning', function (e) { events.push(e); });
    env.call('project.new', { kind: 'subs', pair: 'es-grc', sourcePath: 'mock://data/samples/sample.es.srt' });
    var r = env.call('translate.start', {});
    eq(r.error.code, 'bad_params');
    eq(r.error.hint, 'This language pair is not available yet.');
    env.call('project.new', { kind: 'subs', pair: 'en-la', sourcePath: 'mock://data/samples/sample.en.srt' });
    r = env.call('translate.start', { engines: { rules: true, model: true, online: true } }).result;
    eq(typeof r.jobId, 'number');
    eq(r.total, 12);
    deepEq(r.warnings, ['model_missing', 'online_disabled']);
    env.clock.tick(500);
    deepEq(events.map(function (e) { return e.engine + ':' + e.code; }), ['model:model_missing', 'online:online_disabled']);
    eq(events[0].jobId, r.jobId);
    eq(events[0].hint, 'The local model is not installed. The rule engine translates on its own.');
    var o = env.call('orbergise.start', { tier: 1 });
    eq(o.error.code, 'bad_params', 'Orbergise needs a Latin project');
    eq(o.error.hint, 'Open a Latin file to orbergise it.');
  });

  it('B8 cue.set {remember} answers correctionAdded as an object; names.list lists only the names set', function () {
    var env = setup();
    env.call('project.new', { kind: 'subs', pair: 'en-la', sourcePath: 'mock://data/samples/sample.en.srt' });
    env.call('translate.start', {});
    env.clock.tick(500);
    var r = env.call('cue.set', { index: 0, text: 'Puella rosam spectat.', remember: 'phrase' }).result;
    deepEq(r.correctionAdded, { id: 'c1', key: 'the girl sees the rose.', target: 'Puella rosam spectat.', scope: 'phrase', count: 1 });
    deepEq(env.call('names.list').result.names, [], 'no detection: Marcus is in the file but not listed');
    env.call('names.set', { name: 'Marcus', policy: 'decline', form: 'Mārcus' });
    var names = env.call('names.list').result.names;
    eq(names.length, 1);
    eq(names[0].count, undefined, 'like the real engine: no count');
  });

  it('B8 la-en: cues flagged source-tokens, cue.get tokens are the Latin source words with analysis reasons and the word-by-word alternative', function () {
    var env = setup();
    env.call('project.new', { kind: 'subs', pair: 'la-en', sourcePath: 'mock://data/samples/sample.la.srt' });
    env.call('translate.start', {});
    env.clock.tick(500);
    var g = env.call('cue.get', { index: 0 }).result;
    eq(g.cue.source, 'Puella rosam videt.');
    eq(g.cue.target, 'The girl sees the rose.');
    ok(g.cue.flags.indexOf('source-tokens') >= 0);
    deepEq(g.tokens.map(function (t) { return t.text; }), ['Puella', 'rosam', 'videt']);
    var an = g.reasons.filter(function (x) { return x.kind === 'analysis'; });
    eq(an.length, 3);
    deepEq(Object.keys(an[1].data).sort(), ['alternatives', 'confidence', 'form', 'gloss', 'glossLang', 'head', 'lemmaId', 'pivot', 'role', 'why']);
    eq(an[1].data.head, 'rosa');
    eq(an[1].data.form, 'noun, accusative singular');
    eq(an[2].data.form, 'verb, third person singular present indicative active');
    eq(an[1].data.role, 'object');
    eq(an[0].data.alternatives.length, 1);
    eq(g.reasons.filter(function (x) { return x.kind === 'evidence'; }).length, 12);
    deepEq(g.alternatives, [{ text: 'girl rose see', reason: 'word by word', score: 0.5 }]);
    deepEq(g.checks.map(function (c) { return c.id; }), ['A1', 'ambiguity']);
    var wl = env.call('words.list').result;
    ok(wl.words.some(function (x) { return x.lemma.id === 'puella'; }), 'words.list counts the Latin side');
  });

  it('B8 Greek: en-grc sample ends with a Greek question, Greek tokens, lemma.get cells with the dual and alternative spellings', function () {
    var env = setup();
    env.call('project.new', { kind: 'subs', pair: 'en-grc', sourcePath: 'mock://data/samples/sample.en.srt' });
    env.call('translate.start', {});
    env.clock.tick(500);
    var page = env.call('cue.page', { from: 0, count: 12 }).result.cues;
    eq(page[0].target, 'ἡ κόρη τὸ ῥόδον ὁρᾷ.');
    eq(page[11].target, 'πῶς ἔχεις, ὦ φίλε;', 'the Greek question mark is ";"');
    var g = env.call('cue.get', { index: 0 }).result;
    var kore = g.tokens.filter(function (t) { return t.text === 'κόρη'; })[0];
    eq(kore.lemmaId, 'kore');
    var cells = env.call('lemma.get', { lang: 'grc', id: 'kore' }).result.cells;
    eq(cells.length, 15);
    eq(cells.filter(function (c) { return c.features.number === 'dual'; }).length, 5);
    var verb = env.call('lemma.get', { lang: 'grc', id: 'horao' }).result.cells;
    var third = verb.filter(function (c) { return c.features.person === 'third' && c.features.number === 'singular' && c.features.tense === 'present' && c.features.voice === 'active'; });
    deepEq(third.map(function (c) { return c.form; }), ['ὁρᾷ', 'ὁράει']);
    deepEq(third[0].extra, ['attic', 'contracted']);
    var wi = env.call('word.inspect', { text: 'ὁρᾷ', lang: 'grc' }).result;
    eq(wi.analyses[0].lemma.head, 'ὁράω');
  });
});
