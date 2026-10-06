describe('VP_Panes', function () {
  function setup() {
    var env = load('all');
    var W = env.window;
    W.VP_I18n.load('en-US', readJson('i18n/en-US.json'));
    W.VP_I18n.load('es-MX', readJson('i18n/es-MX.json'));
    W.VP_I18n.setLang('en-US');
    W.VP_Debug.enable(true);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_Bridge.init({ mock: true });
    W.VP_Keys.bind(env.document);
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.call = function (cmd, params) {
      var out = { result: null, error: null };
      call(cmd, params).then(function (r) { out.result = r; }, function (e) { out.error = e; });
      env.clock.tick(20);
      return out;
    };
    W.VP_Store.set('settings', env.call('settings.get').result);
    env.box = W.VP_Dom.el('div');
    env.document.body.appendChild(env.box);
    return env;
  }
  // A project whose cues are all in the store, the panes mounted, cue `index` selected.
  function ready(env, params, index, kind) {
    var W = env.window;
    var r = params.path ? env.call('project.open', params) : env.call('project.new', params);
    var p = W.VP_Start.normalizeProject(r.result.project);
    W.VP_Store.set('project', p);
    var page = env.call('cue.page', { from: 0, count: Math.min(200, p.cues) }).result;
    W.VP_Store.putCues(page.cues);
    W.VP_Store.setCueTotal(p.cues);
    W.VP_Panes.mount(env.box, { kind: kind || p.kind, pair: p.pair });
    W.VP_Store.set('selection', { index: index });
    env.clock.tick(50);
    env.sent.length = 0;
    return W.VP_Panes;
  }
  function q(env, sel) { return env.document.querySelector(sel); }
  function translated(env, from) {
    for (var i = from || 0; i < 40; i++) { if (env.window.VP_Store.getCue(i).state === 'translated') { return i; } }
    return -1;
  }

  it('renders the source with markup: italic tags as italic, ASS overrides as chips, line breaks', function () {
    var env = setup();
    var W = env.window;
    var box = W.VP_Dom.el('div');
    W.VP_Panes.renderMarkup(box, '{\\an8}<i>Quick!</i> The bus\\Nis {\\i1}late{\\i0}.');
    var chips = box.querySelectorAll('.vp-markup-chip');
    deepEq(chips.map(function (c) { return c.textContent; }), ['{\\an8}', '{\\i1}', '{\\i0}']);
    eq(box.querySelectorAll('.vp-i')[0].textContent, 'Quick!');
    eq(box.querySelectorAll('.vp-i')[1].textContent, 'late');
    eq(box.querySelectorAll('br').length, 1);
    eq(box.textContent, '{\\an8}Quick! The busis {\\i1}late{\\i0}.');
    eq(env.innerHTMLWrites, 0);
  });

  it('shows the selected cue: timing, duration, cps chip in warn colour over the limit, word chips, preview and alternatives', function () {
    var env = setup();
    var W = env.window;
    var Pn = ready(env, { path: 'C:\\x\\Fabula.vpoeta' }, 0);
    var i = translated(env);
    W.VP_Store.set('selection', { index: i });
    env.clock.tick(50);
    var c = W.VP_Store.getCue(i);
    eq(Pn.mode(), 'view');
    eq(q(env, '#vp-src-text').textContent, c.source);
    eq(q(env, '#vp-src-title').textContent, 'Source (English)');
    eq(q(env, '#vp-tgt-title').textContent, 'Translation (Latin)');
    ok(q(env, '.vp-timing').textContent.indexOf('\u2192') > 0, 'timing shown with an arrow');
    ok(/ s$/.test(q(env, '.vp-dur').textContent));
    eq(q(env, '#vp-target-view').getAttribute('lang'), 'la');
    var words = env.document.querySelectorAll('.vp-word');
    eq(words.length, c.target.match(/[^\s.,;:?!]+/g).length);
    eq(q(env, '#vp-target-view').textContent.replace(/[\ud83c-\ud83e][\udc00-\udfff]|[\u2600-\u27bf]\ufe0f?/g, ''), c.target);
    var offered = env.call('cue.get', { index: i }).result.alternatives.filter(function (a) { return a.text !== c.target; });
    ok(offered.length >= 1);
    eq(Pn.alternatives().length, Math.min(3, offered.length), 'the current wording is not offered again');
    eq(env.document.querySelectorAll('.vp-alt').length, Pn.alternatives().length);
    ok(env.document.querySelectorAll('.vp-preview-line').length >= 1);
    var fast = JSON.parse(JSON.stringify(c));
    fast.cps = 25.5;
    fast.flags = ['cps'];
    W.VP_Store.putCues([fast]);
    env.clock.tick(50);
    ok(q(env, '.vp-cps').classList.contains('vp-cps-fast'), 'warn colour');
    eq(q(env, '.vp-cps').textContent, 'Fast: 25.5 chars/s');
    ok(q(env, '.vp-cps').getAttribute('title').indexOf('The limit is 17') > 0);
    ok(q(env, '.vp-preview-warn').textContent.indexOf('Too fast: 25.5 characters per second') === 0);
    var long = JSON.parse(JSON.stringify(fast));
    long.target = 'Puella rosam videt et nauta in īnsulā habitat et agricola aquam portat et canis dormit.';
    long.lines = [long.target];
    W.VP_Store.putCues([long]);
    env.clock.tick(50);
    var lines = env.document.querySelectorAll('.vp-preview-line');
    ok(lines.length === 2 || q(env, '.vp-preview-over'), 're-broken into two lines or marked');
    ok(q(env, '.vp-preview-warn').textContent.indexOf('longer than 42') > 0, q(env, '.vp-preview-warn').textContent);
    ok(lines[0].textContent.indexOf('insula') > 0 || lines[1].textContent.indexOf('insula') >= 0, 'export default: no macrons in the preview');
  });

  it('editor state machine: E edits, Esc cancels without a call, Ctrl+Enter keeps through cue.set, then the remember chip', function () {
    var env = setup();
    var W = env.window;
    var Pn = ready(env, { path: 'C:\\x\\Fabula.vpoeta' }, 0);
    var i = translated(env);
    W.VP_Store.set('selection', { index: i });
    env.clock.tick(50);
    var before = W.VP_Store.getCue(i).target;
    eq(Pn.mode(), 'view');
    ok(Pn.startEdit());
    eq(Pn.mode(), 'edit');
    var input = q(env, '#vp-editor');
    eq(input.value, before);
    eq(env.document.activeElement, input);
    ok(q(env, '#vp-target-view').hidden);
    ok(Pn.cancelEdit());
    eq(Pn.mode(), 'view');
    eq(env.sent.filter(function (s) { return s.cmd === 'cue.set'; }).length, 0, 'cancel sends nothing');
    eq(Pn.cancelEdit(), false, 'cancel only while editing');
    Pn.startEdit();
    var same = null;
    Pn.acceptEdit().then(function (r) { same = r; });
    env.clock.tick(20);
    eq(same, false, 'unchanged text is not sent');
    eq(Pn.mode(), 'view');
    Pn.startEdit();
    Pn.setEditorText('Puella rosam amat.');
    var kept = null;
    Pn.acceptEdit().then(function (r) { kept = r; });
    eq(Pn.mode(), 'saving');
    env.clock.tick(50);
    eq(kept, true);
    eq(Pn.mode(), 'remember');
    var set = env.sent.filter(function (s) { return s.cmd === 'cue.set'; });
    eq(set.length, 1);
    deepEq(set[0].params, { index: i, text: 'Puella rosam amat.' });
    eq(W.VP_Store.getCue(i).target, 'Puella rosam amat.');
    eq(W.VP_Store.getCue(i).state, 'edited');
    ok(!q(env, '.vp-remember').hidden, 'remember chip shown');
    eq(env.document.activeElement.getAttribute('data-remember'), 'phrase');
    q(env, '[data-remember="phrase"]').click();
    env.clock.tick(50);
    eq(Pn.mode(), 'view');
    set = env.sent.filter(function (s) { return s.cmd === 'cue.set'; });
    deepEq(set[1].params, { index: i, text: 'Puella rosam amat.', remember: 'phrase' });
    eq(env.call('corrections.list').result.corrections.length, 1);
    eq(W.VP_History.size(), 1, 'remembering adds no undo step');
    ok(env.document.querySelector('.vp-toast-text').textContent.indexOf('Saved to your corrections') === 0);
    Pn.startEdit();
    Pn.setEditorText('Puella rosam videt.');
    Pn.acceptEdit();
    env.clock.tick(50);
    eq(Pn.mode(), 'remember');
    q(env, '[data-remember="cue"]').click();
    eq(Pn.mode(), 'view');
    eq(env.sent.filter(function (s) { return s.cmd === 'cue.set'; }).length, 3, 'just this cue: nothing more to send');
  });

  it('unknown words are underlined while typing (word.inspect, debounced) with suggestions', function () {
    var env = setup();
    var Pn = ready(env, { path: 'C:\\x\\Fabula.vpoeta' }, 0);
    Pn.startEdit();
    Pn.setEditorText('Puella rosam xyzzy videt.');
    env.clock.tick(100);
    eq(env.sent.filter(function (s) { return s.cmd === 'word.inspect'; }).length, 0, 'debounced');
    env.clock.tick(400);
    var asked = env.sent.filter(function (s) { return s.cmd === 'word.inspect'; }).map(function (s) { return s.params.text; });
    deepEq(asked.sort(), ['Puella', 'rosam', 'videt', 'xyzzy']);
    var marks = env.document.querySelectorAll('.vp-editor-mirror .vp-unknown');
    deepEq(marks.map(function (m) { return m.textContent; }), ['xyzzy']);
    eq(q(env, '.vp-editor-mirror').textContent.replace('\u200b', ''), 'Puella rosam xyzzy videt.', 'the mirror holds the same text');
    ok(q(env, '.vp-editor-unknown').textContent.indexOf('“xyzzy” is not in the dictionary.') === 0, q(env, '.vp-editor-unknown').textContent);
    env.sent.length = 0;
    Pn.setEditorText('Puella rosam xyzzy videt in xyzzy.');
    env.clock.tick(400);
    deepEq(env.sent.filter(function (s) { return s.cmd === 'word.inspect'; }).map(function (s) { return s.params.text; }), ['in'], 'cached words are not asked again');
    eq(env.document.querySelectorAll('.vp-editor-mirror .vp-unknown').length, 2);
  });

  it('a word chip publishes VP_Store "inspect" and opens a small card from word.inspect; Esc-style dismiss closes it', function () {
    var env = setup();
    var W = env.window;
    var Pn = ready(env, { kind: 'subs', pair: 'en-la', sourcePath: 'C:\\data\\samples\\sample.en.srt' }, 0);
    env.call('translate.start', { indices: [0] });
    env.clock.tick(500);
    W.VP_Store.putCues(env.call('cue.page', { from: 0, count: 1 }).result.cues);
    env.clock.tick(100);
    var seen = null;
    W.VP_Store.subscribe('inspect', function (v) { seen = v; });
    var word = env.document.querySelectorAll('.vp-word')[1];
    eq(word.textContent, 'rosam');
    word.click();
    deepEq(seen, { index: 0, token: 1, text: 'rosam', lang: 'la', lemmaId: 'rosa' });
    env.clock.tick(50);
    var pop = q(env, '.vp-word-pop');
    ok(!pop.hidden);
    ok(pop.textContent.indexOf('rosa, rosae') >= 0, pop.textContent);
    ok(pop.textContent.indexOf('noun') >= 0);
    ok(pop.textContent.indexOf('“rose”') >= 0);
    ok(pop.textContent.indexOf('Form: accusative, singular') >= 0, pop.textContent);
    W.VP_I18n.setLang('es-MX');
    ok(pop.textContent.indexOf('“rosa”') >= 0 && pop.textContent.indexOf('acusativo') >= 0, 'card follows the language: ' + pop.textContent);
    W.VP_I18n.setLang('en-US');
    ok(Pn.dismiss());
    ok(pop.hidden);
    env.key(env.document.querySelectorAll('.vp-word')[0], 'Enter');
    ok(!pop.hidden, 'Enter on a word opens it too');
    eq(seen.text, 'Puella');
  });

  it('alternatives 1-3 call cue.choose; macrons and emoji toggles change only the rendering', function () {
    var env = setup();
    var W = env.window;
    var Pn = ready(env, { kind: 'subs', pair: 'en-la', sourcePath: 'C:\\data\\samples\\sample.en.srt' }, 0);
    env.call('translate.start', {});
    env.clock.tick(500);
    W.VP_Store.putCues(env.call('cue.page', { from: 0, count: 12 }).result.cues);
    W.VP_Store.set('selection', { index: 1 });
    env.clock.tick(50);
    var target = W.VP_Store.getCue(1).target;
    eq(target, 'Nauta in īnsulā habitat.');
    ok(q(env, '#vp-target-view').textContent.indexOf('īnsulā') > 0);
    W.VP_Store.patch('settings', { showMacrons: false });
    env.clock.tick(20);
    ok(q(env, '#vp-target-view').textContent.indexOf('insula') > 0, 'display without macrons');
    eq(W.VP_Store.getCue(1).target, target, 'data unchanged');
    W.VP_Store.patch('settings', { showMacrons: true });
    W.VP_Store.set('selection', { index: 0 });
    env.clock.tick(50);
    ok(q(env, '.vp-word-emoji'), 'emoji after a picturable noun');
    W.VP_Store.patch('settings', { showEmoji: false });
    env.clock.tick(20);
    eq(q(env, '.vp-word-emoji'), null);
    W.VP_Store.set('selection', { index: 1 });
    env.clock.tick(50);
    var alt = Pn.alternatives()[0];
    ok(alt && alt.text !== target);
    ok(Pn.chooseAlt(1));
    env.clock.tick(100);
    deepEq(env.sent.filter(function (s) { return s.cmd === 'cue.choose'; })[0].params, { index: 1, alternative: alt.engineIndex });
    eq(W.VP_Store.getCue(1).target, alt.text);
    eq(Pn.chooseAlt(3), false, 'no third alternative');
    Pn.startEdit();
    eq(Pn.chooseAlt(1), false, 'not while editing');
  });

  it('leaving a cue while editing keeps a changed text; text projects have no timing and no preview', function () {
    var env = setup();
    var W = env.window;
    var Pn = ready(env, { path: 'C:\\x\\Fabula.vpoeta' }, 0);
    var i = translated(env);
    W.VP_Store.set('selection', { index: i });
    env.clock.tick(50);
    Pn.startEdit();
    Pn.setEditorText('Salvēte, discipulī.');
    W.VP_Store.set('selection', { index: i + 1 });
    env.clock.tick(50);
    eq(W.VP_Store.getCue(i).target, 'Salvēte, discipulī.', 'kept, not lost');
    eq(Pn.mode(), 'view');
    W.VP_Panes.destroy();
    W.VP_Store.set('selection', null);
    var t = ready(env, { kind: 'text', pair: 'es-la', text: 'La niña ve la rosa.\n\nEl lobo corre.' }, 1, 'text');
    eq(q(env, '.vp-timing'), null);
    eq(q(env, '.vp-preview'), null);
    eq(q(env, '#vp-src-text').textContent, 'El lobo corre.');
    eq(q(env, '#vp-src-title').textContent, 'Source (Spanish)');
    ok(q(env, '.vp-target-empty').textContent.indexOf('Not translated yet') === 0);
    ok(t.startEdit(), 'an untranslated cue can be written by hand');
  });

  it('destroy() returns every count to its baseline; caches are capped', function () {
    var env = setup();
    var W = env.window;
    var before = W.VP_Router.counts();
    ready(env, { path: 'C:\\x\\Fabula.vpoeta' }, 0);
    for (var i = 0; i < 40; i++) {
      W.VP_Store.set('selection', { index: i });
      env.clock.tick(30);
    }
    ok(W.VP_Panes.stats().details <= 41, 'cue.get cache capped');
    W.VP_Panes.startEdit();
    W.VP_Panes.destroy();
    env.clock.tick(1000);
    deepEq(W.VP_Router.counts(), before);
    eq(env.box.childNodes.length, 0);
    deepEq(W.VP_I18n.missing(), []);
    eq(env.errors().length, 0);
  });
});
