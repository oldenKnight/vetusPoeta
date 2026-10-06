describe('VP_Inspector', function () {
  // The whole app over the mock engine, the sample open and translated, cue `index` selected.
  function boot() {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    env.app = D.el('div', { id: 'app', className: 'vp-app' });
    env.main = D.el('main', { id: 'vp-main' });
    env.app.appendChild(env.main);
    env.document.body.appendChild(env.app);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_App.boot({ root: env.main, status: D.el('div') });
    env.clock.tick(500);
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.cmds = function (name) { return env.sent.filter(function (s) { return s.cmd === name; }); };
    env.q = function (sel) { return env.document.querySelector(sel); };
    env.qa = function (sel) { return env.document.querySelectorAll(sel); };
    env.call = function (cmd, params) {
      var out = { result: null, error: null };
      call(cmd, params).then(function (r) { out.result = r; }, function (e) { out.error = e; });
      env.clock.tick(20);
      return out;
    };
    W.VP_Start.openSample();
    env.clock.tick(300);
    env.q('#vp-translate').click();
    env.clock.tick(3000);
    W.VP_Toast.clearAll();
    W.VP_CueList.setFilter('all');
    W.VP_CueList.select(0);
    env.clock.tick(100);
    return env;
  }
  function inspect(env, k) {
    env.window.VP_Panes.openWord(k);
    env.clock.tick(100);
  }
  function text(env, sel) { var n = env.q(sel); return n ? n.textContent : null; }

  it('renders the four "why this word" blocks, the headword, tier badge, emoji, gloss and the form in words from a cue.get payload', function () {
    var env = boot();
    var W = env.window;
    eq(W.VP_Workspace.tab(), 'word');
    ok(text(env, '#vp-panel-body').indexOf('Click a word of the translation') >= 0, 'empty state');
    ok(env.qa('.vp-insp-srcword').length >= 4, 'source words of the cue are listed');
    inspect(env, 0);
    var st = W.VP_Inspector.state();
    eq(st.word, 'Puella');
    ok(st.loaded);
    eq(text(env, '.vp-insp-word'), 'puella');
    eq(env.q('.vp-insp-word').getAttribute('lang'), 'la');
    eq(text(env, '.vp-insp-pos'), 'noun');
    eq(env.q('.vp-insp-head .vp-tier').getAttribute('aria-label'), 'Level 1 of 3: basic word');
    eq(env.qa('.vp-insp-head .vp-leaf-on').length, 3, 'T1 = three filled leaves');
    eq(text(env, '.vp-insp-emoji'), '👧');
    eq(text(env, '.vp-insp-gloss-text'), '“girl”');
    ok(text(env, '.vp-insp-uses').indexOf('nominative singular feminine (nom. sg. f.)') > 0, text(env, '.vp-insp-uses'));
    eq(env.qa('[data-insp-help]').length, 3, 'a Grammar help link per term');
    var why = env.q('#vp-why');
    ok(why.hidden, 'why starts closed');
    var heads = env.qa('.vp-why-block h3').map(function (h) { return h.textContent; });
    deepEq(heads, ['Meaning', 'Candidates', 'Form', 'Evidence']);
    ok(text(env, '.vp-why-block').indexOf('girl') >= 0);
    var cands = env.qa('.vp-why-cand');
    eq(cands.length, 2);
    ok(cands[0].classList.contains('vp-why-chosen'));
    eq(cands[0].querySelector('.vp-why-mark').textContent, 'chosen');
    eq(cands[1].querySelector('.vp-why-cand-head').textContent, 'virgō');
    ok(cands[1].textContent.indexOf('rarer word') > 0, 'one-line reason');
    var ev = env.qa('.vp-why-evidence .vp-ev').map(function (li) { return li.className.replace('vp-ev ', ''); });
    deepEq(ev, ['vp-ev-yes', 'vp-ev-yes', 'vp-ev-off', 'vp-ev-off']);
    ok(text(env, '.vp-why-evidence').indexOf('Local model: off') >= 0 || text(env, '.vp-why-evidence').indexOf('Local modeloff') >= 0);
    eq(env.qa('.vp-why-checklist .vp-ev').length, 9, 'A1..A9');
    W.VP_Inspector.toggleWhy(true);
    ok(!why.hidden);
    env.key(env.document.body, 'w');
    ok(why.hidden, 'W toggles "Why this word?"');
    eq(env.q('[data-insp-action="why"]').getAttribute('aria-expanded'), 'false');
    eq(W.VP_I18n.missing().length, 0);
    eq(env.errors().length, 0);
  });

  it('"Other forms" is lazy: lemma.get only when opened; the table highlights the used cell; verbs get person/number x tense per mood and voice', function () {
    var env = boot();
    var W = env.window;
    inspect(env, 0);
    eq(env.cmds('lemma.get').length, 0, 'no lemma.get before opening');
    ok(env.q('#vp-insp-forms').hidden);
    env.q('[data-insp-action="forms"]').click();
    env.clock.tick(50);
    eq(env.cmds('lemma.get').length, 1);
    deepEq(env.cmds('lemma.get')[0].params, { lang: 'la', id: 'puella' });
    var table = env.q('.vp-paradigm');
    ok(table, 'paradigm table');
    eq(table.querySelector('caption').textContent, 'Forms of puella');
    var cols = table.querySelectorAll('thead th').map(function (th) { return th.textContent; });
    deepEq(cols, ['sg.', 'pl.']);
    var rows = table.querySelectorAll('tbody th').map(function (th) { return th.textContent; });
    deepEq(rows, ['nom.', 'voc.', 'acc.', 'gen.', 'dat.', 'abl.']);
    var used = table.querySelectorAll('.vp-par-used');
    eq(used.length, 1);
    eq(used[0].querySelector('.vp-text').textContent, 'puella');
    eq(table.querySelectorAll('tbody tr')[2].querySelectorAll('td')[1].textContent, 'puellās');
    W.VP_Inspector.toggleForms(false);
    ok(env.q('#vp-insp-forms').hidden);
    eq(env.q('#vp-insp-forms').childNodes.length, 0, 'closed table is dropped from the DOM');
    inspect(env, 2);
    eq(W.VP_Inspector.state().word, 'videt');
    W.VP_Inspector.toggleForms(true);
    env.clock.tick(50);
    var sel = env.q('#vp-par-set');
    ok(sel, 'mood/voice picker for verbs');
    eq(sel.value, 'indicative|active');
    table = env.q('.vp-paradigm');
    deepEq(table.querySelectorAll('thead th').map(function (th) { return th.textContent; }), ['pres.', 'impf.', 'fut.', 'perf.', 'plup.']);
    deepEq(table.querySelectorAll('tbody th').map(function (th) { return th.textContent; }).slice(0, 3), ['1st sg.', '2nd sg.', '3rd sg.']);
    eq(table.querySelector('.vp-par-used .vp-text').textContent, 'videt');
    ok(text(env, '.vp-par-other').indexOf('vidēre') >= 0, 'infinitives listed as other forms');
    eq(env.cmds('lemma.get').length, 2);
    W.VP_Inspector.toggleForms(false);
    W.VP_Inspector.toggleForms(true);
    eq(env.cmds('lemma.get').length, 2, 'cached');
  });

  it('grid() is pure: nouns by case x number, adjectives add the gender, verbs by mood|voice groups', function () {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_inspector.js']);
    var g = env.window.VP_Inspector.grid([
      { features: { pos: 'adj', 'case': 'nominative', number: 'singular', gender: 'masculine' }, form: 'clārus' },
      { features: { pos: 'adj', 'case': 'nominative', number: 'singular', gender: 'feminine' }, form: 'clāra' },
      { features: { pos: 'adj', 'case': 'accusative', number: 'plural', gender: 'neuter' }, form: 'clāra' },
      { features: { pos: 'adj', 'case': 'nominative', number: 'singular', gender: 'masculine', degree: 'comparative' }, form: 'clārior' }
    ], { 'case': 'nominative', number: 'singular', gender: 'feminine' });
    eq(g.kind, 'nominal');
    deepEq(g.rows, ['nominative', 'accusative']);
    deepEq(g.cols.map(function (c) { return c.key; }), ['singular|masculine', 'singular|feminine', 'plural|neuter']);
    eq(g.used, 'nominative#singular|feminine');
    eq(g.forms['nominative#singular|masculine'].length, 1, 'comparative cells are left out of the positive table');
    var v = env.window.VP_Inspector.grid([
      { features: { pos: 'verb', person: 'third', number: 'singular', tense: 'present', mood: 'indicative', voice: 'passive' }, form: 'vidētur' },
      { features: { pos: 'verb', person: 'first', number: 'plural', tense: 'perfect', mood: 'indicative', voice: 'active' }, form: 'vīdimus' },
      { features: { pos: 'verb', tense: 'present', mood: 'infinitive', voice: 'active' }, form: 'vidēre' }
    ], { person: 'third', number: 'singular', tense: 'present', mood: 'indicative', voice: 'passive' });
    eq(v.kind, 'verbal');
    deepEq(v.groups.map(function (x) { return x.key; }), ['indicative|active', 'indicative|passive']);
    eq(v.usedGroup, 'indicative|passive');
    eq(v.used, 'third|singular#present');
    eq(v.other.length, 1);
    eq(env.window.VP_Inspector.replaceToken('Puella rosam videt.', [{ text: 'Puella' }, { text: 'rosam' }, { text: 'videt' }], 0, 'virgō'), 'Virgō rosam videt.');
    deepEq(env.window.VP_Inspector.formWords({ pos: 'noun', 'case': 'acc', number: 'pl' }).terms, [{ feature: 'case', value: 'accusative' }, { feature: 'number', value: 'plural' }], 'abbreviated engine values are normalised');
  });

  it('"Use another word" replaces the token through cue.set and marks the cue Check; "Add to my corrections" remembers the phrase; the help dialog explains a term', function () {
    var env = boot();
    var W = env.window;
    inspect(env, 0);
    eq(W.VP_Store.getCue(0).confidence, 'ok');
    var btn = env.q('[data-insp-action="another"]');
    ok(!btn.disabled);
    btn.click();
    ok(!env.q('#vp-insp-choices').hidden);
    eq(env.document.activeElement, env.q('[data-insp-choice="0"]'));
    env.q('[data-insp-choice="0"]').click();
    env.clock.tick(100);
    var set = env.cmds('cue.set');
    eq(set.length, 1);
    eq(set[0].params.text, 'Virgō rosam videt.');
    eq(W.VP_Store.getCue(0).target, 'Virgō rosam videt.');
    eq(W.VP_Store.getCue(0).state, 'edited');
    eq(W.VP_Store.getCue(0).confidence, 'check', 'marked Check until re-checked');
    eq(W.VP_History.canUndo(), true);
    env.clock.tick(200);
    eq(W.VP_Inspector.state().word, 'Virgō', 'the inspector follows the new word');
    env.q('[data-insp-action="correction"]').click();
    env.clock.tick(100);
    set = env.cmds('cue.set');
    eq(set.length, 2);
    eq(set[1].params.remember, 'phrase');
    eq(env.call('corrections.list').result.corrections.length, 1);
    env.q('[data-insp-help]').click();
    eq(W.VP_Dialog.count(), 1);
    eq(env.q('.vp-dialog-title').textContent, 'nominative');
    ok(env.q('.vp-help').textContent.indexOf('case of the subject') > 0, 'one-paragraph explanation');
    W.VP_Dialog.closeAll();
    eq(env.errors().length, 0);
  });

  it('source-word clicks show the chosen sense and the Latin candidates; Back returns; caches are capped and dropped on destroy', function () {
    var env = boot();
    var W = env.window;
    env.qa('.vp-insp-srcword').filter(function (b) { return b.textContent === 'girl'; })[0].click();
    env.clock.tick(100);
    var st = W.VP_Inspector.state();
    eq(st.side, 'source');
    eq(st.word, 'girl');
    ok(text(env, '.vp-insp-uses').indexOf('Puella') > 0, 'translated here as');
    ok(env.qa('.vp-why-cand').length >= 1, 'Latin candidates');
    env.q('[data-insp-target]').click();
    env.clock.tick(100);
    eq(W.VP_Inspector.state().side, 'target');
    eq(W.VP_Inspector.state().word, 'Puella');
    W.VP_Inspector.show({ index: 0, token: -1, text: 'Quick', lang: 'en', side: 'source' });
    env.clock.tick(100);
    ok(text(env, '#vp-panel-body').indexOf('gave no meaning') > 0);
    env.q('[data-insp-action="back"]').click();
    env.clock.tick(50);
    eq(W.VP_Inspector.state().word, null);
    for (var i = 0; i < 12; i++) {
      W.VP_CueList.select(i);
      env.clock.tick(30);
      inspect(env, 0);
    }
    ok(W.VP_Inspector.stats().cueGet <= 20);
    var before = W.VP_Router.counts();
    W.VP_Workspace.setTab('engines');
    eq(W.VP_Inspector.isMounted(), false);
    W.VP_Workspace.setTab('word');
    W.VP_Workspace.close();
    env.clock.tick(300);
    deepEq(W.VP_Debug.failures(), []);
    ok(W.VP_Router.counts().listeners < before.listeners);
  });
});

describe('Reading pairs and Greek (B8)', function () {
  // The whole app over the mock, the sample of `pair` open and translated, cue 0 selected.
  function bootPair(pair) {
    var env = load('all', { search: '?mock=1&debug=1' });
    var W = env.window;
    var D = W.VP_Dom;
    env.app = D.el('div', { id: 'app', className: 'vp-app' });
    env.main = D.el('main', { id: 'vp-main' });
    env.app.appendChild(env.main);
    env.document.body.appendChild(env.app);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_App.boot({ root: env.main, status: D.el('div') });
    env.clock.tick(500);
    env.sent = [];
    var call = W.VP_Bridge.call;
    W.VP_Bridge.call = function (cmd, params) {
      env.sent.push({ cmd: cmd, params: params });
      return call(cmd, params);
    };
    env.cmds = function (name) { return env.sent.filter(function (s) { return s.cmd === name; }); };
    env.q = function (sel) { return env.document.querySelector(sel); };
    env.qa = function (sel) { return env.document.querySelectorAll(sel); };
    env.q('#vp-start-pair').value = pair;
    env.fire(env.q('#vp-start-pair'), 'change');
    W.VP_Start.openSample();
    env.clock.tick(300);
    env.q('#vp-translate').click();
    env.clock.tick(3000);
    W.VP_Toast.clearAll();
    W.VP_CueList.setFilter('all');
    W.VP_CueList.select(0);
    env.clock.tick(200);
    return env;
  }

  it('la-en: source words are chips with a hover card, the target is plain readable text, a click shows the reading in the Word tab, Ctrl+I adds the interlinear lines', function () {
    var env = bootPair('la-en');
    var W = env.window;
    eq(W.VP_Store.get('project').pair, 'la-en');
    eq(env.q('#vp-src-text').textContent, 'Puella rosam videt.');
    var chips = env.qa('#vp-src-text .vp-src-word');
    deepEq(chips.map(function (c) { return c.textContent; }), ['Puella', 'rosam', 'videt']);
    eq(env.q('#vp-src-text').getAttribute('lang'), 'la');
    eq(env.qa('#vp-target-view .vp-word').length, 0, 'no chips on the English side');
    eq(env.q('#vp-target-view').textContent, 'The girl sees the rose.');
    ok(W.VP_Panes.reading());
    eq(env.q('.vp-il-btn').hidden, false, 'the interlinear toggle is offered');
    eq(env.q('.vp-il-btn').getAttribute('aria-pressed'), 'false', 'off by default');
    // hover card
    env.fire(chips[1], 'mouseover', { bubbles: true });
    env.clock.tick(50);
    var card = env.q('#vp-src-card');
    eq(card.hidden, false);
    ok(card.textContent.indexOf('rosa') === 0, card.textContent);
    ok(card.textContent.indexOf('Form: accusative singular') >= 0, card.textContent);
    ok(card.textContent.indexOf('“rose”') >= 0, card.textContent);
    ok(card.textContent.indexOf('Role in the sentence: direct object') >= 0, card.textContent);
    ok(card.querySelector('.vp-tier'), 'tier badge');
    ok(card.querySelector('.vp-emoji') && card.querySelector('.vp-emoji').textContent === '🌹', 'emoji');
    W.VP_I18n.setLang('es-MX');
    ok(card.textContent.indexOf('“rosa”') >= 0 && card.textContent.indexOf('acusativo') >= 0, 'the card follows the UI language: ' + card.textContent);
    W.VP_I18n.setLang('en-US');
    env.fire(env.qa('#vp-src-text .vp-src-word')[1], 'mouseout', { bubbles: true });
    eq(card.hidden, true, 'the card goes when the pointer leaves the word');
    // click -> Word tab with the reading
    var seen = null;
    W.VP_Store.subscribe('inspect', function (v) { seen = v; });
    env.qa('#vp-src-text .vp-src-word')[1].click();
    env.clock.tick(100);
    deepEq(seen, { index: 0, token: 1, text: 'rosam', lang: 'la', lemmaId: 'rosa', side: 'analysis' });
    eq(W.VP_Workspace.tab(), 'word');
    var body = env.q('#vp-panel-body');
    eq(body.querySelector('.vp-insp-word').textContent, 'rosa');
    eq(body.querySelector('.vp-insp-word').getAttribute('lang'), 'la');
    eq(body.querySelector('[data-insp-action="why"]').textContent, 'Why this reading?');
    W.VP_Inspector.toggleWhy(true);
    var blocks = body.querySelectorAll('.vp-why-block');
    eq(blocks.length, 3, 'reading, other readings, evidence');
    ok(blocks[0].textContent.indexOf('rosa as accusative singular') >= 0, blocks[0].textContent);
    ok(blocks[0].textContent.indexOf('The only reading that fits (100 %).') >= 0, blocks[0].textContent);
    ok(blocks[1].textContent.indexOf('No other reading fits this form.') >= 0);
    eq(body.querySelectorAll('[data-insp-action="another"]').length, 0, 'no "Use another word" for a source word');
    W.VP_Inspector.toggleForms(true);
    env.clock.tick(100);
    ok(body.querySelector('.vp-paradigm .vp-par-used'), 'the paradigm of the Latin word, used cell marked');
    // a word with two readings lists the other one
    env.qa('#vp-src-text .vp-src-word')[0].click();
    env.clock.tick(100);
    W.VP_Inspector.toggleWhy(true);
    ok(env.q('.vp-why-alts').textContent.indexOf('Puella (puella, noun, vocative singular)') >= 0);
    ok(env.q('.vp-why-reading').textContent.indexOf('Confidence: 66 %') >= 0, env.q('.vp-why-reading').textContent);
    // Ctrl+I: interlinear lines
    env.key(env.document.body, 'i', { ctrl: true });
    env.clock.tick(100);
    eq(W.VP_Store.get('settings').interlinear, true);
    eq(env.q('.vp-il-btn').getAttribute('aria-pressed'), 'true');
    var il = env.qa('#vp-src-text .vp-src-word')[1];
    eq(il.querySelector('.vp-il-word').textContent, 'rosam');
    eq(il.querySelector('.vp-il-lemma').textContent, 'rosa');
    eq(il.querySelector('.vp-il-form').textContent, 'acc. sg. f.');
    eq(il.querySelector('.vp-il-gloss').textContent, 'rose');
    env.key(env.document.body, 'i', { ctrl: true });
    env.clock.tick(100);
    eq(env.qa('#vp-src-text .vp-il-word').length, 0, 'Ctrl+I again hides them');
    // alternatives: the word-by-word line with its reason in words
    ok(env.q('.vp-alt-reason') && env.q('.vp-alt-reason').textContent === 'word by word', 'reason "word by word" translated by key');
    W.VP_Toast.clearAll();
    deepEq(W.VP_I18n.missing(), []);
    eq(env.errors().length, 0, JSON.stringify(env.errors()));
  });

  it('en-grc: Greek spans carry lang="grc", the root swaps to the Greek accent, the ";" question mark stays; the paradigm keeps Attic forms and drops the dual', function () {
    var env = bootPair('en-grc');
    var W = env.window;
    ok(env.q('.vp-ws').classList.contains('pair-grc'));
    ok(env.app.classList.contains('pair-grc'), 'dialogs and toasts under the app get the accent too');
    eq(env.q('#vp-target-view').getAttribute('lang'), 'grc');
    eq(env.q('.vp-preview-line').getAttribute('lang'), 'grc');
    W.VP_CueList.select(11);
    env.clock.tick(200);
    eq(env.q('#vp-target-view').textContent, 'πῶς ἔχεις, ὦ φίλε;');
    ok(env.qa('.vp-cue-row [lang="grc"]').length > 0, 'cue list rows show Greek with lang="grc"');
    W.VP_CueList.select(0);
    env.clock.tick(200);
    var words = env.qa('#vp-target-view .vp-word');
    var kore = words.filter(function (x) { return x.textContent === 'κόρη'; })[0];
    kore.click();
    env.clock.tick(100);
    W.VP_Inspector.toggleForms(true);
    env.clock.tick(100);
    var table = env.q('.vp-paradigm');
    var rows = table.querySelectorAll('tbody tr').map(function (r) { return r.querySelector('th').getAttribute('title'); });
    deepEq(rows, ['nominative', 'genitive', 'dative', 'accusative', 'vocative']);
    var cols = table.querySelectorAll('thead th').map(function (h) { return h.getAttribute('title'); });
    deepEq(cols, ['singular', 'plural'], 'the dual is left out');
    eq(table.querySelector('.vp-par-used').textContent.indexOf('κόρη'), 0);
    eq(table.querySelector('.vp-par-used span').getAttribute('lang'), 'grc');
    W.VP_Toast.clearAll();
    eq(env.errors().length, 0, JSON.stringify(env.errors()));
  });

  it('grid(cells, used, "grc"): Attic contracted spelling chosen, dual dropped, empty voice = middle-passive; Latin unchanged', function () {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_inspector.js']);
    var I = env.window.VP_Inspector;
    var f = function (o) { return { pos: 'verb', 'case': '', number: o.n || '', gender: '', person: o.p || '', tense: o.t || '', mood: o.m || '', voice: o.v === undefined ? 'active' : o.v, degree: '' }; };
    var cells = [
      { features: f({ p: 'third', n: 'singular', t: 'present', m: 'indicative' }), form: 'ὁράει', extra: [] },
      { features: f({ p: 'third', n: 'singular', t: 'present', m: 'indicative' }), form: 'ὁρᾷ', extra: ['attic', 'contracted'] },
      { features: f({ p: 'third', n: 'singular', t: 'present', m: 'indicative' }), form: 'ὁρᾰ́ει', extra: ['attic'] },
      { features: f({ p: 'third', n: 'dual', t: 'present', m: 'indicative' }), form: 'ὁρᾶτον', extra: ['attic', 'contracted'] },
      { features: f({ p: 'third', n: 'singular', t: 'aorist', m: 'indicative' }), form: 'εἶδε', extra: ['attic'] },
      { features: f({ t: 'present', m: 'infinitive', v: '' }), form: 'ὁρᾶσθαι', extra: ['attic', 'contracted'] }
    ];
    var g = I.grid(cells, { person: 'third', number: 'singular', tense: 'present', mood: 'indicative', voice: 'active' }, 'grc');
    eq(g.kind, 'verbal');
    var grp = g.groups[0];
    deepEq(grp.rows, ['third|singular'], 'no dual row');
    deepEq(grp.cols, ['present', 'aorist']);
    deepEq(grp.forms['third|singular#present'], ['ὁρᾷ']);
    eq(g.used, 'third|singular#present');
    eq(g.other[0].voice, 'mediopassive');
    var plain = cells.slice(0, 3).map(function (c) { return { features: c.features, form: c.form }; });
    var lat = I.grid(plain, {}, 'la');
    deepEq(lat.groups[0].forms['third|singular#present'], ['ὁράει', 'ὁρᾷ', 'ὁρᾰ́ει'], 'cells without marks: every spelling as before');
  });
});
