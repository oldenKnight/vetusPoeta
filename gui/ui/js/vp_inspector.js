/* vp_inspector.js - the Word tab of the right panel (DESIGN 13; PREDESIGN 1.2.1, 4.5, 4.6).
 *
 * Follows VP_Store 'inspect' {index, token, text, lang, lemmaId, side?, orberg?} published by
 * VP_Panes (target word chips) and VP_Orberg (changed words), and its own list of the cue's
 * source words (side "source"). For a target word: headword with macrons, part of speech,
 * tier badge (laurel leaves + words), emoji, dictionary line (gloss in the UI language, "(via
 * English)" when LemmaView.flags has "gloss-es-pivot"), "This cue uses: <form in words>
 * (<abbreviations>)" with a Grammar help link per term, "Other forms" (paradigm table from
 * lemma.get cells, built only when opened, the used cell highlighted), and "Why this word?"
 * with the four blocks of PREDESIGN 4.5 fed by cue.get reasons of that token (Meaning,
 * Candidates, Form, Evidence) plus the cue's checks. Actions: "Use another word" (a
 * candidate replaces the token through cue.set; the cue shows Check until re-checked) and
 * "Add to my corrections" (cue.set with remember "phrase"). Every analysis line is engine
 * data; the UI only words it. Caches: cue.get 20, word.inspect 200, lemma.get 20 (LRU).
 *
 * VP_Inspector.mount(el, params) / destroy(); show(inspect); toggleWhy(open?) -> bool;
 * toggleForms(open?) -> bool; useCandidate(n) -> Promise; addCorrection() -> Promise;
 * openHelp(feature, value); state(); helpers shared with the other panels: tierBadge(tier,
 * compact), formWords(features) -> {words, abbr, terms}, glossOf(lemma), grid(cells,
 * features, lang) (pure, tests), replaceToken(text, tokens, k, word), stats()
 * B8: a source word of a reading pair (la-en, la-es, grc-en, grc-es; inspect side
 * "analysis") shows its lexicon entry, the form it was read as, the paradigm, and "Why this
 * reading?" from its `analysis` reason {head, gloss, form, role, confidence, alternatives,
 * why} (chosen reading, other readings, evidence, checks); it has no "Use another word".
 * Greek paradigms (lang grc): cases nominative, genitive, dative, accusative, vocative; the
 * dual is left out; of the spellings greek.vpl lists for one cell, the Attic (and contracted)
 * ones are shown; a verb voice left empty in the data is the middle-passive.
 */
(function () {
  'use strict';

  var OWNER = 'inspector';
  var CUE_CAP = 20;
  var WORD_CAP = 200;
  var LEMMA_CAP = 20;
  var SOURCE_WORDS = 40;
  var WORD_RE = /[^\s.,;:?!¿¡"“”«»()\[\]{}\-–—·;]+/g;
  var CASES = ['nominative', 'vocative', 'accusative', 'genitive', 'dative', 'ablative', 'locative'];
  var NUMBERS = ['singular', 'dual', 'plural'];
  var GENDERS = ['masculine', 'feminine', 'neuter', 'masculine-feminine', 'masculine-neuter', 'feminine-neuter', 'common', ''];
  var PERSONS = ['first', 'second', 'third'];
  var TENSES = ['present', 'imperfect', 'future', 'perfect', 'pluperfect', 'future-perfect', 'aorist'];
  var MOODS = ['indicative', 'subjunctive', 'optative', 'imperative', 'infinitive', 'participle', 'gerund', 'gerundive', 'supine'];
  var VOICES = ['active', 'middle', 'mediopassive', 'passive'];
  var GREEK_CASES = ['nominative', 'genitive', 'dative', 'accusative', 'vocative'];
  var GREEK_TENSES = ['present', 'imperfect', 'future', 'aorist', 'perfect', 'pluperfect', 'future-perfect'];
  var CLASSICAL = { la: true, grc: true };
  var MODERN = { en: true, es: true };
  var NOMINAL = ['case', 'number', 'gender', 'degree'];
  var VERBAL = ['person', 'number', 'tense', 'mood', 'voice'];
  var NORMAL = { '1': 'first', '2': 'second', '3': 'third', m: 'masculine', f: 'feminine', n: 'neuter', sg: 'singular', pl: 'plural', du: 'dual',
    nom: 'nominative', voc: 'vocative', acc: 'accusative', gen: 'genitive', dat: 'dative', abl: 'ablative', loc: 'locative',
    pres: 'present', impf: 'imperfect', fut: 'future', perf: 'perfect', plup: 'pluperfect', ind: 'indicative', subj: 'subjunctive', imp: 'imperative', inf: 'infinitive',
    act: 'active', pass: 'passive', mid: 'middle', comp: 'comparative', sup: 'superlative' };
  var SOURCES = ['wiktionary', 'whitaker', 'model', 'online'];
  var CHECKS = ['A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9'];

  var s = null;
  var whyOpen = false;

  // ---------------------------------------------------------------- small helpers
  function lru(cap) {
    var keys = [];
    var map = {};
    return {
      get: function (k) {
        if (!Object.prototype.hasOwnProperty.call(map, k)) { return undefined; }
        keys.splice(keys.indexOf(k), 1);
        keys.push(k);
        return map[k];
      },
      put: function (k, v) {
        if (Object.prototype.hasOwnProperty.call(map, k)) { keys.splice(keys.indexOf(k), 1); }
        keys.push(k);
        map[k] = v;
        while (keys.length > cap) { delete map[keys.shift()]; }
      },
      drop: function (k) {
        if (Object.prototype.hasOwnProperty.call(map, k)) {
          keys.splice(keys.indexOf(k), 1);
          delete map[k];
        }
      },
      size: function () { return keys.length; },
      clear: function () {
        keys = [];
        map = {};
      }
    };
  }

  function settings() { return window.VP_Store.get('settings') || {}; }

  function display(text) {
    var t = String(text || '');
    return settings().showMacrons === false && window.VP_CueList ? window.VP_CueList.stripMacrons(t) : t;
  }

  function camel(v) { return String(v).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }

  function norm(v) {
    var x = String(v || '').toLowerCase();
    return NORMAL[x] || x;
  }

  function T(key, vars) { return window.VP_I18n.t(key, vars); }

  function termKey(feature, value, kind) { return 'grammar.' + feature + '.' + camel(norm(value)) + '.' + kind; }

  function term(feature, value, kind) {
    var key = termKey(feature, value, kind || 'label');
    return window.VP_I18n.has(key) ? T(key) : String(value);
  }

  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }

  function txt(tag, cls, text, extra) {
    var a = { className: cls, text: text };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function i18nEl(tag, cls, key, vars) {
    return el(tag, { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) });
  }

  function pairLangs() {
    var p = window.VP_Store.get('project') || {};
    var parts = String(p.pair || 'en-la').split('-');
    return { src: parts[0], dst: parts[1] || parts[0] };
  }

  // Latin or Greek read into English or Spanish: the words to explain are the source words.
  function readingPair() {
    var l = pairLangs();
    return CLASSICAL[l.src] === true && MODERN[l.dst] === true;
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function P() { return window.Promise; }

  // ---------------------------------------------------------------- shared helpers
  // Tier badge (PREDESIGN 4.6): T1 = 3 filled laurel leaves, T2 = 2, T3 = 1, with the word.
  function tierBadge(tier, compact) {
    var n = Number(tier);
    if (!(n >= 1 && n <= 3)) { return null; }
    var leaves = [];
    for (var i = 1; i <= 3; i++) {
      leaves.push(el('svg', { className: 'vp-leaf' + (i <= 4 - n ? ' vp-leaf-on' : ''), 'aria-hidden': 'true', focusable: 'false', viewBox: '0 0 16 16' }, [el('use', { href: '#vp-i-leaf' })]));
    }
    return el('span', { className: 'vp-tier vp-tier-t' + n + (compact ? ' vp-tier-compact' : ''), role: 'img', 'data-i18n-aria': 'tier.t' + n + '.aria', 'aria-label': T('tier.t' + n + '.aria'), title: T('tier.t' + n + '.aria') }, [
      el('span', { className: 'vp-tier-leaves' }, leaves),
      compact ? null : el('span', { className: 'vp-tier-label', 'data-i18n': 'tier.t' + n + '.label', text: T('tier.t' + n + '.label') })
    ]);
  }

  // A FeatureView in words first, abbreviations second (PREDESIGN 1.2.1).
  function formWords(f) {
    f = f || {};
    var verb = !!(f.person || f.tense || (f.mood && f.mood !== 'participle'));
    var order = verb ? VERBAL : NOMINAL;
    var words = [];
    var abbr = [];
    var terms = [];
    for (var i = 0; i < order.length; i++) {
      var v = f[order[i]];
      if (!v || (order[i] === 'degree' && norm(v) === 'positive')) { continue; }
      words.push(term(order[i], v, 'label'));
      abbr.push(term(order[i], v, 'abbr'));
      terms.push({ feature: order[i], value: norm(v) });
    }
    return { words: words.join(' '), abbr: abbr.join(' '), terms: terms };
  }

  function glossOf(l) {
    l = l || {};
    var es = window.VP_I18n.lang() === 'es-MX';
    var flags = l.flags || [];
    if (es && l.glossEs) { return { text: l.glossEs, note: flags.indexOf('gloss-es-pivot') >= 0 ? 'inspector.gloss.viaEnglish.label' : null }; }
    if (!es && l.glossEn) { return { text: l.glossEn, note: null }; }
    if (l.glossEn) { return { text: l.glossEn, note: 'inspector.gloss.english.label' }; }
    return { text: l.glossEs || '', note: null };
  }

  // Token positions by text search (engine offsets are UTF-8 bytes), as VP_Panes does.
  function tokenAt(text, tokens, k) {
    var pos = 0;
    for (var i = 0; i < tokens.length; i++) {
      var at = String(text).indexOf(tokens[i].text, pos);
      if (at < 0) { return -1; }
      if (i === k) { return at; }
      pos = at + tokens[i].text.length;
    }
    return -1;
  }

  function replaceToken(text, tokens, k, word) {
    var at = tokenAt(text, tokens, k);
    if (at < 0) { return null; }
    var old = tokens[k].text;
    var w = String(word);
    if (/^[A-ZĀĒĪŌŪȲ]/.test(old) && w) { w = w.charAt(0).toUpperCase() + w.slice(1); }
    return text.slice(0, at) + w + text.slice(at + old.length);
  }

  function rank(list, v) {
    var i = list.indexOf(v);
    return i < 0 ? list.length : i;
  }

  function same(a, b) { return norm(a) === norm(b); }

  // The paradigm grid of lemma.get cells. Nouns, adjectives, pronouns: cases x number (and
  // gender when the cells carry it). Verbs: one table per mood and voice, person/number x
  // tense; forms without a person (infinitives, participles) go to `other`.
  function grid(cells, used, lang) {
    used = used || {};
    var greek = lang === 'grc';
    var list = [];
    var verbal = false;
    var i;
    for (i = 0; i < (cells || []).length; i++) {
      var f = cells[i].features || {};
      if (greek && norm(f.number) === 'dual') { continue; }
      list.push(cells[i]);
      if (f.person || f.tense) { verbal = true; }
    }
    var scores = {};
    return verbal ? verbalGrid(list, used, greek, scores) : nominalGrid(list, used, greek, scores);
  }

  // Of several spellings of one cell the best scored are kept: Attic and contracted first,
  // "alternative" last (greek.vpl marks them in `extra`; Latin cells score alike).
  function cellScore(cell) {
    var ex = (cell && cell.extra) || [];
    return (ex.indexOf('attic') >= 0 ? 2 : 0) + (ex.indexOf('contracted') >= 0 ? 1 : 0) - (ex.indexOf('alternative') >= 0 ? 4 : 0);
  }

  function addForm(map, key, form, score, scores) {
    score = score || 0;
    if (!scores || scores[key] === undefined || score > scores[key]) {
      map[key] = [form];
      if (scores) { scores[key] = score; }
      return;
    }
    if (score === scores[key] && map[key].indexOf(form) < 0) { map[key].push(form); }
  }

  function nominalGrid(cells, used, greek, scores) {
    var rows = [];
    var cols = [];
    var colKeys = {};
    var forms = {};
    var usedKey = null;
    var genders = {};
    var i;
    for (i = 0; i < cells.length; i++) {
      var g = norm(cells[i].features && cells[i].features.gender);
      if (g) { genders[g] = true; }
    }
    var byGender = Object.keys(genders).length > 1;
    for (i = 0; i < cells.length; i++) {
      var f = cells[i].features || {};
      var deg = norm(f.degree);
      if (deg && deg !== 'positive' && deg !== norm(used.degree || 'positive')) { continue; }
      var c = norm(f['case']);
      var n = norm(f.number);
      if (!c || !n) { continue; }
      var gk = byGender ? norm(f.gender) : '';
      var ck = n + '|' + gk;
      if (rows.indexOf(c) < 0) { rows.push(c); }
      if (!colKeys[ck]) {
        colKeys[ck] = true;
        cols.push({ key: ck, number: n, gender: gk });
      }
      addForm(forms, c + '#' + ck, cells[i].form, cellScore(cells[i]), scores);
      if (same(c, used['case']) && same(n, used.number) && (!gk || !used.gender || same(gk, used.gender))) { usedKey = c + '#' + ck; }
    }
    var caseOrder = greek ? GREEK_CASES : CASES;
    rows.sort(function (a, b) { return rank(caseOrder, a) - rank(caseOrder, b); });
    cols.sort(function (a, b) { return rank(NUMBERS, a.number) - rank(NUMBERS, b.number) || rank(GENDERS, a.gender) - rank(GENDERS, b.gender); });
    return { kind: 'nominal', rows: rows, cols: cols, forms: forms, used: usedKey };
  }

  function verbalGrid(cells, used, greek, scores) {
    var groups = {};
    var order = [];
    var other = [];
    var otherAt = {};
    var usedGroup = null;
    var usedKey = null;
    var noVoice = greek ? 'mediopassive' : 'active';
    var tenseOrder = greek ? GREEK_TENSES : TENSES;
    for (var i = 0; i < cells.length; i++) {
      var f = cells[i].features || {};
      var mood = norm(f.mood) || 'indicative';
      var voice = norm(f.voice) || noVoice;
      var score = cellScore(cells[i]);
      if (!f.person) {
        var ok = [norm(f.tense), mood, voice, norm(f.gender)].join('|');
        var isUsed = !used.person && same(f.tense, used.tense) && same(mood, used.mood || 'indicative') && same(voice, used.voice || noVoice);
        if (otherAt[ok] === undefined) {
          otherAt[ok] = other.length;
          other.push({ tense: norm(f.tense), mood: mood, voice: voice, gender: norm(f.gender), form: cells[i].form, used: isUsed, score: score });
        } else if (score > other[otherAt[ok]].score) {
          other[otherAt[ok]].form = cells[i].form;
          other[otherAt[ok]].score = score;
        }
        continue;
      }
      var gk = mood + '|' + voice;
      if (!groups[gk]) {
        groups[gk] = { key: gk, mood: mood, voice: voice, rows: [], cols: [], forms: {}, scores: {} };
        order.push(gk);
      }
      var g = groups[gk];
      var rk = norm(f.person) + '|' + norm(f.number);
      var t = norm(f.tense);
      if (g.rows.indexOf(rk) < 0) { g.rows.push(rk); }
      if (g.cols.indexOf(t) < 0) { g.cols.push(t); }
      addForm(g.forms, rk + '#' + t, cells[i].form, score, g.scores);
      if (same(f.person, used.person) && same(f.number, used.number) && same(t, used.tense) && same(mood, used.mood || 'indicative') && same(voice, used.voice || noVoice)) {
        usedGroup = gk;
        usedKey = rk + '#' + t;
      }
    }
    var list = order.map(function (k) {
      var g = groups[k];
      g.rows.sort(function (a, b) {
        var x = a.split('|');
        var y = b.split('|');
        return rank(NUMBERS, x[1]) - rank(NUMBERS, y[1]) || rank(PERSONS, x[0]) - rank(PERSONS, y[0]);
      });
      g.cols.sort(function (a, b) { return rank(tenseOrder, a) - rank(tenseOrder, b); });
      return g;
    });
    list.sort(function (a, b) { return rank(MOODS, a.mood) - rank(MOODS, b.mood) || rank(VOICES, a.voice) - rank(VOICES, b.voice); });
    return { kind: 'verbal', groups: list, other: other, usedGroup: usedGroup || (list[0] ? list[0].key : null), used: usedKey };
  }

  // ---------------------------------------------------------------- data
  function wordKey(text, lang) { return lang + '|' + String(text).toLowerCase(); }

  function fetchCue(index) {
    var hit = s.cues.get(index);
    if (hit) { return P().resolve(hit); }
    return window.VP_Bridge.call('cue.get', { index: index }).then(function (r) {
      if (s) { s.cues.put(index, r); }
      return r;
    });
  }

  function fetchWord(text, lang) {
    var k = wordKey(text, lang);
    var hit = s.words.get(k);
    if (hit) { return P().resolve(hit); }
    return window.VP_Bridge.call('word.inspect', { text: text, lang: lang }).then(function (r) {
      if (s) { s.words.put(k, r); }
      return r;
    });
  }

  function fetchLemma(lang, id) {
    var k = lang + '|' + id;
    var hit = s.lemmas.get(k);
    if (hit) { return P().resolve(hit); }
    return window.VP_Bridge.call('lemma.get', { lang: lang, id: id }).then(function (r) {
      if (s) { s.lemmas.put(k, r); }
      return r;
    });
  }

  function tokenOf(detail, x) {
    var toks = (detail && detail.tokens) || [];
    var t = toks[x.token];
    if (t && t.text === x.text) { return { tok: t, k: x.token }; }
    for (var i = 0; i < toks.length; i++) { if (toks[i].text === x.text) { return { tok: toks[i], k: i }; } }
    return { tok: { text: x.text, display: x.text }, k: -1 };
  }

  function reasonsFor(detail, k) {
    return ((detail && detail.reasons) || []).filter(function (r) { return r && r.tokenIndex === k; });
  }

  function pickAnalysis(word, lemmaId) {
    var list = (word && word.analyses) || [];
    for (var i = 0; i < list.length; i++) {
      if (lemmaId !== null && lemmaId !== undefined && list[i].lemma && String(list[i].lemma.id) === String(lemmaId)) { return list[i]; }
    }
    return list[0] || null;
  }

  // ---------------------------------------------------------------- rendering
  function setBody(kids) {
    var D = window.VP_Dom;
    D.clear(s.body);
    D.append(s.body, kids);
    s.formsEl = D.qs('.vp-insp-forms', s.body);
    s.whyEl = D.qs('.vp-why', s.body);
    s.choiceEl = D.qs('.vp-insp-choices', s.body);
  }

  function sourceWords(c) {
    var words = String((c && c.source) || '').replace(/<[^>]*>|\{\\[^}]*\}/g, ' ').match(WORD_RE) || [];
    var seen = {};
    var out = [];
    for (var i = 0; i < words.length && out.length < SOURCE_WORDS; i++) {
      var k = words[i].toLowerCase();
      if (seen[k] || /^\d+$/.test(k)) { continue; }
      seen[k] = true;
      out.push(words[i]);
    }
    return out;
  }

  function renderEmpty() {
    var sel = window.VP_Store.get('selection');
    var c = sel ? window.VP_Store.getCue(sel.index) : null;
    var kids = [
      i18nEl('h2', 'vp-panel-title', 'inspector.empty.title'),
      i18nEl('p', 'vp-hint', 'inspector.empty.hint')
    ];
    var words = sourceWords(c);
    if (c && words.length) {
      var langs = pairLangs();
      kids.push(i18nEl('h3', 'vp-insp-sub', 'inspector.source.title'));
      kids.push(i18nEl('p', 'vp-hint', readingPair() ? 'inspector.source.reading.hint' : 'inspector.source.hint'));
      kids.push(el('div', { className: 'vp-insp-srcwords' }, words.map(function (w) {
        return el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-insp-srcword', lang: langs.src, 'data-insp-src': w, text: w });
      })));
    }
    setBody(kids);
  }

  function header(word, lemma, lang, tok) {
    var l = lemma || {};
    var headParts = String(l.head || '').split(/,\s*/);
    var citation = headParts[0] || tok.display || tok.text;
    var showEmoji = settings().showEmoji !== false;
    var emoji = tok.emoji || l.emoji;
    var g = glossOf(l);
    var kids = [
      el('div', { className: 'vp-insp-head' }, [
        el('h2', { className: 'vp-insp-word vp-text', lang: lang, text: display(citation) }),
        l.pos ? txt('span', 'vp-insp-pos', term('pos', l.pos)) : null,
        tierBadge(l.tier || tok.tier),
        emoji && showEmoji ? el('span', { className: 'vp-emoji vp-insp-emoji', role: 'img', 'aria-label': T('inspector.emoji.aria', { word: citation }), text: emoji }) : null
      ])
    ];
    if (g.text) {
      kids.push(el('p', { className: 'vp-insp-gloss' }, [
        txt('span', 'vp-insp-gloss-text', T('inspector.gloss.label', { gloss: g.text })),
        g.note ? i18nEl('span', 'vp-hint vp-insp-note', g.note) : null
      ]));
    } else if (lemma) {
      kids.push(i18nEl('p', 'vp-hint', 'inspector.gloss.none.label'));
    }
    var entry = l.principal || l.head;
    if (entry && entry !== citation) {
      kids.push(el('p', { className: 'vp-insp-entry' }, [
        i18nEl('span', 'vp-hint', 'inspector.entry.label'),
        txt('span', 'vp-text vp-insp-entry-text', display(entry), { lang: lang })
      ]));
    }
    return kids;
  }

  function usesBlock(tok, features, lang) {
    var fw = formWords(features);
    var kids = [el('p', { className: 'vp-insp-uses' }, [
      i18nEl('span', 'vp-insp-label', 'inspector.uses.label'),
      txt('b', 'vp-text', display(tok.display || tok.text), { lang: lang }),
      txt('span', 'vp-insp-form', fw.words ? T('inspector.form.label', { words: fw.words, abbr: fw.abbr }) : T('inspector.form.none.label'))
    ])];
    if (fw.terms.length) {
      kids.push(el('p', { className: 'vp-insp-help' }, [i18nEl('span', 'vp-hint', 'inspector.help.label')].concat(fw.terms.map(function (t) {
        var label = term(t.feature, t.value, 'label');
        return el('button', { type: 'button', className: 'vp-link', 'data-insp-help': t.feature + ':' + t.value, 'aria-label': T('inspector.help.aria', { term: label }), text: label });
      }))));
    }
    return kids;
  }

  function evidenceRow(reasons) {
    var by = {};
    reasons.forEach(function (r) {
      var d = r.data || {};
      if (r.kind === 'evidence' && d.source) { by[d.source] = d.state || (d.ok === true ? 'yes' : (d.ok === false ? 'no' : 'none')); }
    });
    return el('ul', { className: 'vp-why-evidence' }, SOURCES.map(function (src) {
      var st = by[src] || 'none';
      return el('li', { className: 'vp-ev vp-ev-' + st }, [
        i18nEl('span', 'vp-ev-source', 'why.evidence.source.' + src + '.label'),
        i18nEl('span', 'vp-ev-state', 'why.evidence.state.' + st + '.label')
      ]);
    }));
  }

  function checksRow(checks) {
    var list = (checks || []).filter(function (c) { return CHECKS.indexOf(c.id) >= 0; });
    if (!list.length) { return null; }
    return el('div', { className: 'vp-why-checks' }, [
      i18nEl('span', 'vp-hint', 'why.checks.label'),
      el('ul', { className: 'vp-why-checklist' }, list.map(function (c) {
        return el('li', { className: 'vp-ev vp-ev-' + (c.ok ? 'yes' : 'no'), title: c.detail || null }, [i18nEl('span', null, 'why.check.' + c.id.toLowerCase() + '.label')]);
      }))
    ]);
  }

  function meaningLines(reasons) {
    var out = [];
    reasons.forEach(function (r) {
      var d = r.data || {};
      if (r.kind === 'sense') {
        if (d.source && d.sense) {
          out.push(txt('p', 'vp-why-line', T('why.meaning.sense.label', { source: d.source, sense: d.sense })));
        } else if (r.text) {
          out.push(txt('p', 'vp-why-line', r.text));
        }
        if (d.context && d.context.length) { out.push(txt('p', 'vp-hint', T('why.meaning.context.label', { list: d.context.join(', ') }))); }
      } else if (r.kind === 'correction' || r.kind === 'phrasebook' || r.kind === 'name') {
        out.push(el('p', { className: 'vp-why-line vp-why-' + r.kind }, [i18nEl('span', null, 'why.' + r.kind + '.label'), r.text ? txt('span', 'vp-hint', ' ' + r.text) : null]));
      } else if (r.kind === 'evidence' && d.source === 'model' && d.preferred) {
        out.push(i18nEl('p', 'vp-why-line', 'why.model.label'));
      }
    });
    if (!out.length) { out.push(i18nEl('p', 'vp-hint', 'why.meaning.none.label')); }
    return out;
  }

  function candidates(reasons) {
    return reasons.filter(function (r) { return r.kind === 'candidate' && r.data && !r.data.was; });
  }

  function candidateList(reasons, lang) {
    var list = candidates(reasons);
    if (!list.length) { return [i18nEl('p', 'vp-hint', 'why.candidates.none.label')]; }
    return [el('ol', { className: 'vp-why-cands' }, list.map(function (r) {
      var d = r.data;
      var band = d.band && window.VP_I18n.has('why.candidate.band.' + d.band + '.label') ? T('why.candidate.band.' + d.band + '.label') : '';
      return el('li', { className: 'vp-why-cand' + (d.chosen ? ' vp-why-chosen' : '') }, [
        txt('span', 'vp-text vp-why-cand-head', display(String(d.head || d.form || '').split(/,\s*/)[0]), { lang: lang }),
        tierBadge(d.tier, true),
        band ? txt('span', 'vp-hint', band) : null,
        d.chosen ? i18nEl('span', 'vp-why-mark', 'why.candidate.chosen.label') : null,
        r.text ? txt('span', 'vp-why-reason', r.text) : null
      ]);
    }))];
  }

  function formLines(reasons, features) {
    var out = [];
    reasons.forEach(function (r) {
      if (r.kind !== 'form') { return; }
      var f = (r.data && (r.data.features || r.data)) || null;
      var fw = formWords(f);
      if (fw.words) { out.push(txt('p', 'vp-why-line', T('inspector.form.label', { words: fw.words, abbr: fw.abbr }))); }
      if (r.text) { out.push(txt('p', 'vp-hint', r.text)); }
    });
    if (!out.length) {
      var fw2 = formWords(features);
      out.push(txt('p', 'vp-why-line', fw2.words ? T('inspector.form.label', { words: fw2.words, abbr: fw2.abbr }) : T('inspector.form.none.label')));
    }
    return out;
  }

  function orbergBlock(change) {
    if (!change) { return null; }
    var why = change.why && window.VP_I18n.has('why.orberg.reason.' + change.why + '.label') ? T('why.orberg.reason.' + change.why + '.label') : (change.text || '');
    return el('section', { className: 'vp-why-block vp-why-orberg' }, [
      i18nEl('h3', null, 'why.orberg.title'),
      el('p', { className: 'vp-why-line vp-text', lang: 'la' }, [txt('span', 'vp-orb-was', display(change.was)), txt('span', 'vp-orb-arrow', ' → ', { 'aria-hidden': 'true' }), txt('span', 'vp-orb-now', display(change.now))]),
      why ? txt('p', 'vp-hint', why) : null
    ]);
  }

  function whyBlock(reasons, checks, features, lang) {
    return el('div', { id: 'vp-why', className: 'vp-why', hidden: !whyOpen }, [
      el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.meaning.title')].concat(meaningLines(reasons))),
      el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.candidates.title')].concat(candidateList(reasons, lang))),
      el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.form.title')].concat(formLines(reasons, features))),
      el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.evidence.title'), evidenceRow(reasons), checksRow(checks)])
    ]);
  }

  // "Why this reading?" for a source word of a reading pair, from its analysis reason.
  function analysisWhy(reasons, a, checks, features, lang) {
    var fw = formWords(features);
    var conf = typeof a.confidence === 'number' ? Math.round(Math.max(0, Math.min(1, a.confidence)) * 100) : null;
    var role = window.VP_Panes && typeof window.VP_Panes.roleText === 'function' ? window.VP_Panes.roleText(a.role) : (a.role || '');
    var chosen = [
      el('p', { className: 'vp-why-line' }, [
        txt('b', 'vp-text', display(a.head || ''), { lang: lang }),
        txt('span', null, ' ' + T('why.reading.form.label', { form: fw.words || a.form || '' })),
        a.gloss ? txt('span', 'vp-why-gloss', ' ' + T('why.reading.gloss.label', { gloss: a.gloss })) : null
      ]),
      role ? txt('p', 'vp-why-line', T('source.word.role.label', { role: role })) : null,
      conf !== null ? txt('p', 'vp-hint', T(conf >= 100 ? 'why.reading.sure.label' : 'why.reading.confidence.label', { pct: conf })) : null
    ].concat((a.why || []).map(function (w) { return txt('p', 'vp-hint vp-why-engine', String(w)); }));
    var alts = a.alternatives || [];
    return el('div', { id: 'vp-why', className: 'vp-why', hidden: !whyOpen }, [
      el('section', { className: 'vp-why-block vp-why-reading' }, [i18nEl('h3', null, 'why.reading.title')].concat(chosen)),
      el('section', { className: 'vp-why-block vp-why-others' }, [i18nEl('h3', null, 'why.reading.others.title')].concat(alts.length ?
        [el('ul', { className: 'vp-why-alts' }, alts.map(function (x) { return txt('li', 'vp-why-alt', String(x && x.text !== undefined ? x.text : x)); }))] :
        [i18nEl('p', 'vp-hint', 'why.reading.others.none.label')])),
      el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.evidence.title'), evidenceRow(reasons), checksRow(checks)])
    ]);
  }

  function analysisReason(reasons) {
    for (var i = 0; i < reasons.length; i++) { if (reasons[i].kind === 'analysis') { return reasons[i].data || {}; } }
    return null;
  }

  function expander(id, key, open, action) {
    return el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-expander', 'aria-expanded': open ? 'true' : 'false', 'aria-controls': id, 'data-insp-action': action, 'data-i18n': key, text: T(key) });
  }

  function renderTarget(x, detail, word) {
    var lang = x.lang || pairLangs().dst;
    var found = tokenOf(detail, x);
    var tok = found.tok;
    var a = pickAnalysis(word, x.lemmaId !== undefined && x.lemmaId !== null ? x.lemmaId : tok.lemmaId);
    var lemma = a ? a.lemma : null;
    var features = tok.features || (a && a.features) || {};
    var reasons = reasonsFor(detail, found.k);
    var reading = analysisReason(reasons);
    if (!lemma && reading && reading.head) {
      // the dictionary did not answer for this spelling: the engine's reading stands in
      lemma = { id: reading.lemmaId === undefined ? null : reading.lemmaId, head: reading.head, tier: tok.tier || 0, glossEn: reading.glossLang === 'es' ? '' : reading.gloss, glossEs: reading.glossLang === 'es' ? reading.gloss : '', flags: [] };
      a = { lemma: lemma, features: features };
    }
    s.view = { x: x, k: found.k, tok: tok, lemma: lemma, features: features, reasons: reasons, lang: lang };
    var kids = header(word, lemma, lang, tok);
    if (!a) {
      kids.push(txt('p', 'vp-insp-unknown', T('inspector.unknown.label', { word: tok.text })));
      if (word && word.suggestions && word.suggestions.length) { kids.push(txt('p', 'vp-hint', T('inspector.suggest.label', { list: word.suggestions.slice(0, 3).join(', ') }))); }
    }
    var change = x.orberg || null;
    if (!change) {
      for (var i = 0; i < reasons.length; i++) { if (reasons[i].data && reasons[i].data.was) { change = { was: reasons[i].data.was, now: reasons[i].data.now, why: reasons[i].data.why, text: reasons[i].text }; } }
    }
    kids.push(orbergBlock(change));
    kids = kids.concat(usesBlock(tok, features, lang));
    var tableOk = !!(lemma && lemma.id !== undefined && lemma.id !== null);
    if (tableOk) {
      kids.push(expander('vp-insp-forms', 'inspector.forms.cta', s.formsOpen, 'forms'));
      kids.push(el('div', { id: 'vp-insp-forms', className: 'vp-insp-forms', hidden: true }));
    }
    if (reading || x.side === 'analysis') {
      kids.push(expander('vp-why', 'inspector.whyReading.cta', whyOpen, 'why'));
      kids.push(analysisWhy(reasons, reading || {}, detail && detail.checks, features, lang));
      s.others = [];
      setBody([el('div', { className: 'vp-insp-card vp-insp-reading' }, kids)]);
      if (s.formsOpen && tableOk) { toggleForms(true); }
      return;
    }
    kids.push(expander('vp-why', 'inspector.why.cta', whyOpen, 'why'));
    kids.push(whyBlock(reasons, detail && detail.checks, features, lang));
    var others = candidates(reasons).filter(function (r) { return !r.data.chosen; });
    var editable = x.side !== 'source' && found.k >= 0 && detail && detail.cue && detail.cue.target;
    kids.push(el('div', { className: 'vp-row vp-insp-actions' }, [
      el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-insp-action': 'another', 'aria-expanded': 'false', 'aria-controls': 'vp-insp-choices', disabled: !(editable && others.length), 'data-i18n': 'inspector.another.cta', text: T('inspector.another.cta') }),
      el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-insp-action': 'correction', disabled: !editable, 'data-i18n': 'inspector.correction.cta', text: T('inspector.correction.cta') })
    ]));
    kids.push(el('div', { id: 'vp-insp-choices', className: 'vp-insp-choices', hidden: true }, [
      i18nEl('p', 'vp-hint', 'inspector.another.hint'),
      el('ul', { className: 'vp-insp-choicelist' }, others.map(function (r, n) {
        var d = r.data;
        return el('li', null, [el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-insp-choice', 'data-insp-choice': String(n) }, [
          txt('span', 'vp-text', display(d.form || String(d.head || '').split(/,\s*/)[0]), { lang: lang }),
          tierBadge(d.tier, true),
          d.gloss ? txt('span', 'vp-hint', d.gloss) : null
        ])]);
      }))
    ]));
    s.others = others;
    setBody([el('div', { className: 'vp-insp-card' }, kids)]);
    if (s.formsOpen && tableOk) { toggleForms(true); }
  }

  function renderSource(x, detail) {
    var langs = pairLangs();
    var reasons = (detail && detail.reasons) || [];
    var lower = String(x.text).toLowerCase();
    var hit = null;
    for (var i = 0; i < reasons.length; i++) {
      var d = reasons[i].data || {};
      if (reasons[i].kind === 'sense' && d.source && (String(d.source).toLowerCase() === lower || lower.indexOf(String(d.source).toLowerCase()) === 0)) {
        hit = reasons[i];
        break;
      }
    }
    var kids = [el('div', { className: 'vp-insp-head' }, [
      el('h2', { className: 'vp-insp-word vp-text', lang: langs.src, text: x.text }),
      i18nEl('span', 'vp-insp-pos', 'inspector.sourceWord.label')
    ])];
    s.view = { x: x, k: hit ? hit.tokenIndex : -1, reasons: hit ? reasonsFor(detail, hit.tokenIndex) : [], lang: langs.dst };
    if (!hit) {
      kids.push(i18nEl('p', 'vp-hint', 'inspector.sourceWord.none.label'));
    } else {
      var tok = (detail.tokens || [])[hit.tokenIndex] || { text: '' };
      kids.push(el('p', { className: 'vp-insp-uses' }, [
        i18nEl('span', 'vp-insp-label', 'inspector.sourceWord.as.label'),
        el('button', { type: 'button', className: 'vp-link vp-text', lang: langs.dst, 'data-insp-target': String(hit.tokenIndex), text: display(tok.text) })
      ]));
      kids.push(el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.meaning.title')].concat(meaningLines([hit]))));
      kids.push(el('section', { className: 'vp-why-block' }, [i18nEl('h3', null, 'why.candidates.title')].concat(candidateList(s.view.reasons, langs.dst))));
    }
    kids.push(el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-insp-action': 'back', 'data-i18n': 'inspector.back.cta', text: T('inspector.back.cta') }));
    setBody([el('div', { className: 'vp-insp-card' }, kids)]);
  }

  function renderLoading(x) {
    setBody([el('div', { className: 'vp-insp-card' }, [
      el('h2', { className: 'vp-insp-word vp-text', lang: x.side === 'source' ? pairLangs().src : (x.lang || pairLangs().dst), text: display(x.text) }),
      txt('p', 'vp-hint', T('inspector.loading.label', { word: x.text }))
    ])]);
  }

  function render() {
    if (!s) { return; }
    var x = s.cur;
    if (!x) {
      s.view = null;
      renderEmpty();
      return;
    }
    if (!s.data) {
      renderLoading(x);
      return;
    }
    if (x.side === 'source') { renderSource(x, s.data.detail); } else { renderTarget(x, s.data.detail, s.data.word); }
  }

  // ---------------------------------------------------------------- the paradigm table
  function th(cls, text, title, scope) {
    return el('th', { className: cls || null, scope: scope || null, title: title || null, text: text });
  }

  function cellTd(forms, used, lang) {
    return el('td', { className: used ? 'vp-par-used' : null, title: used ? T('inspector.forms.used.label') : null }, [
      txt('span', 'vp-text', (forms || []).map(display).join(', ') || '–', { lang: lang }),
      used ? i18nEl('span', 'vp-visually-hidden', 'inspector.forms.used.label') : null
    ]);
  }

  function nominalTable(g, lang, head) {
    var hasGender = g.cols.some(function (c) { return !!c.gender; });
    return el('table', { className: 'vp-paradigm' }, [
      el('caption', { text: T('inspector.forms.caption', { head: display(head) }) }),
      el('thead', null, [el('tr', null, [el('td')].concat(g.cols.map(function (c) {
        var a = term('number', c.number, 'abbr') + (hasGender && c.gender ? ' ' + term('gender', c.gender, 'abbr') : '');
        var full = term('number', c.number, 'label') + (hasGender && c.gender ? ' ' + term('gender', c.gender, 'label') : '');
        return th(null, a, full, 'col');
      })))]),
      el('tbody', null, g.rows.map(function (r) {
        return el('tr', null, [th(null, term('case', r, 'abbr'), term('case', r, 'label'), 'row')].concat(g.cols.map(function (c) {
          var key = r + '#' + c.key;
          return cellTd(g.forms[key], key === g.used, lang);
        })));
      }))
    ]);
  }

  function verbalTable(g, lang, head) {
    var group = null;
    for (var i = 0; i < g.groups.length; i++) { if (g.groups[i].key === s.verbGroup) { group = g.groups[i]; } }
    if (!group) { group = g.groups[0]; }
    var kids = [];
    if (g.groups.length > 1) {
      kids.push(el('div', { className: 'vp-field vp-par-pick' }, [
        el('label', { htmlFor: 'vp-par-set', 'data-i18n': 'inspector.forms.set.label', text: T('inspector.forms.set.label') }),
        el('select', { id: 'vp-par-set', className: 'vp-input', 'data-insp-set': '1' }, g.groups.map(function (x) {
          return el('option', { value: x.key, selected: x.key === group.key, text: term('mood', x.mood, 'label') + ' · ' + term('voice', x.voice, 'label') });
        }))
      ]));
      kids[kids.length - 1].childNodes[1].value = group.key;
    }
    if (group) {
      kids.push(el('table', { className: 'vp-paradigm' }, [
        el('caption', { text: T('inspector.forms.caption', { head: display(head) }) }),
        el('thead', null, [el('tr', null, [el('td')].concat(group.cols.map(function (t) { return th(null, term('tense', t, 'abbr'), term('tense', t, 'label'), 'col'); })))]),
        el('tbody', null, group.rows.map(function (r) {
          var pn = r.split('|');
          return el('tr', null, [th(null, term('person', pn[0], 'abbr') + ' ' + term('number', pn[1], 'abbr'), term('person', pn[0], 'label') + ' ' + term('number', pn[1], 'label'), 'row')].concat(group.cols.map(function (t) {
            var key = r + '#' + t;
            return cellTd(group.forms[key], group.key === g.usedGroup && key === g.used, lang);
          })));
        }))
      ]));
    }
    if (g.other.length) {
      kids.push(i18nEl('h4', 'vp-insp-sub', 'inspector.forms.nonfinite.title'));
      kids.push(el('ul', { className: 'vp-par-other' }, g.other.map(function (o) {
        return el('li', { className: o.used ? 'vp-par-used' : null }, [
          txt('span', 'vp-hint', [term('tense', o.tense, 'label'), term('mood', o.mood, 'label'), term('voice', o.voice, 'label'), o.gender ? term('gender', o.gender, 'label') : ''].filter(function (x) { return x; }).join(' ') + ': '),
          txt('span', 'vp-text', display(o.form), { lang: lang })
        ]);
      })));
    }
    return el('div', null, kids);
  }

  function renderForms(r) {
    if (!s || !s.formsEl || !s.view) { return; }
    var D = window.VP_Dom;
    D.clear(s.formsEl);
    var cells = (r && r.cells) || [];
    if (!cells.length) {
      s.formsEl.appendChild(i18nEl('p', 'vp-hint', 'inspector.forms.none.label'));
      return;
    }
    var g = grid(cells, s.view.features, s.view.lang);
    var head = String((r.lemma && r.lemma.head) || (s.view.lemma && s.view.lemma.head) || '').split(/,\s*/)[0];
    if (g.kind === 'verbal' && !s.verbGroup) { s.verbGroup = g.usedGroup; }
    s.grid = g;
    s.formsEl.appendChild(g.kind === 'nominal' ? nominalTable(g, s.view.lang, head) : verbalTable(g, s.view.lang, head));
  }

  function toggleForms(open) {
    if (!s || !s.formsEl || !s.view || !s.view.lemma) { return false; }
    var want = open === undefined ? s.formsEl.hidden : !!open;
    s.formsOpen = want;
    s.formsEl.hidden = !want;
    var btn = window.VP_Dom.qs('[data-insp-action="forms"]', s.body);
    if (btn) { btn.setAttribute('aria-expanded', want ? 'true' : 'false'); }
    if (!want) {
      window.VP_Dom.clear(s.formsEl);
      return false;
    }
    var D = window.VP_Dom;
    D.clear(s.formsEl);
    s.formsEl.appendChild(i18nEl('p', 'vp-hint', 'inspector.forms.loading.label'));
    var gen = s.gen;
    var lemma = s.view.lemma;
    fetchLemma(s.view.lang, lemma.id).then(function (r) {
      if (s && s.gen === gen && s.formsOpen) { renderForms(r); }
    }, function () {
      if (s && s.gen === gen && s.formsOpen) { renderForms(null); }
    });
    return true;
  }

  function toggleWhy(open) {
    whyOpen = open === undefined ? !whyOpen : !!open;
    if (!s || !s.whyEl) { return whyOpen; }
    s.whyEl.hidden = !whyOpen;
    var btn = window.VP_Dom.qs('[data-insp-action="why"]', s.body);
    if (btn) { btn.setAttribute('aria-expanded', whyOpen ? 'true' : 'false'); }
    return whyOpen;
  }

  // ---------------------------------------------------------------- actions
  function markCheck(index) {
    var c = window.VP_Store.getCue(index);
    if (!c || c.confidence !== 'ok') { return; }
    var o = {};
    for (var k in c) { if (Object.prototype.hasOwnProperty.call(c, k)) { o[k] = c[k]; } }
    o.confidence = 'check';
    window.VP_Store.putCues([o]);
  }

  function formFromCells(lemmaId, lang, features) {
    return fetchLemma(lang, lemmaId).then(function (r) {
      var g = grid((r && r.cells) || [], features, lang);
      var forms = g.kind === 'nominal' ? g.forms[g.used] : null;
      if (g.kind === 'verbal') {
        for (var i = 0; i < g.groups.length; i++) { if (g.groups[i].key === g.usedGroup) { forms = g.groups[i].forms[g.used]; } }
      }
      return forms && forms.length ? forms[0] : '';
    });
  }

  function useCandidate(n) {
    if (!s || !s.view || !s.others || !s.others[n] || !s.data || !s.data.detail) { return P().resolve(false); }
    var v = s.view;
    var d = s.others[n].data;
    var detail = s.data.detail;
    var index = v.x.index;
    var gen = s.gen;
    var getForm = d.form ? P().resolve(d.form) : (d.lemmaId !== undefined ? formFromCells(d.lemmaId, v.lang, v.features) : P().resolve(''));
    return getForm.then(function (form) {
      if (!form) {
        window.VP_Toast.show({ key: 'inspector.another.noForm.label', kind: 'error' });
        return false;
      }
      var text = replaceToken(detail.cue.target, detail.tokens || [], v.k, form);
      if (text === null) { return false; }
      return window.VP_Workspace.cmd.edit(index, text).then(function (r) {
        markCheck(index);
        if (s) { s.cues.drop(index); }
        window.VP_Toast.show({ key: 'inspector.another.done.label', kind: 'success' });
        if (s && s.cur && s.cur.index === index) {
          var x = { index: index, token: v.k, text: /^[A-ZĀĒĪŌŪȲ]/.test(v.tok.text) ? form.charAt(0).toUpperCase() + form.slice(1) : form, lang: v.lang, lemmaId: d.lemmaId === undefined ? null : d.lemmaId };
          window.VP_Store.set('inspect', x);
        }
        return !!r;
      });
    }).then(null, function (err) {
      showError(err);
      return false;
    });
  }

  function addCorrection() {
    if (!s || !s.view || !s.data || !s.data.detail || !s.data.detail.cue) { return P().resolve(false); }
    var index = s.view.x.index;
    var c = window.VP_Store.getCue(index) || s.data.detail.cue;
    return window.VP_Workspace.cmd.edit(index, c.target, 'phrase').then(function () {
      if (s) { s.cues.drop(index); }
      window.VP_Toast.show({ key: 'inspector.correction.done.label', kind: 'success' });
      return true;
    }, function (err) {
      showError(err);
      return false;
    });
  }

  function openHelp(feature, value) {
    var label = term(feature, value, 'label');
    var key = termKey(feature, value, 'help');
    var body = el('div', { className: 'vp-help' }, [
      txt('p', 'vp-help-abbr', T('inspector.help.abbr.label', { abbr: term(feature, value, 'abbr') })),
      txt('p', null, window.VP_I18n.has(key) ? T(key) : T('inspector.help.none.label'))
    ]);
    return window.VP_Dialog.open({ title: label, body: body, actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }] });
  }

  // ---------------------------------------------------------------- flow
  function show(x) {
    if (!s) { return; }
    s.gen++;
    s.cur = x || null;
    s.data = null;
    s.formsOpen = false;
    s.verbGroup = null;
    s.grid = null;
    if (!x) {
      render();
      return;
    }
    render();
    var gen = s.gen;
    var wantWord = x.side !== 'source';
    var detailP = fetchCue(x.index).then(null, function () { return null; });
    var wordP = wantWord ? fetchWord(x.text, x.lang || pairLangs().dst).then(null, function () { return { analyses: [], suggestions: [] }; }) : P().resolve(null);
    detailP.then(function (detail) {
      return wordP.then(function (word) {
        if (!s || s.gen !== gen) { return; }
        s.data = { detail: detail, word: word };
        render();
      });
    });
  }

  function onInspect(x) { show(x); }

  function onSelection(sel) {
    if (!s) { return; }
    if (s.cur && sel && s.cur.index === sel.index) { return; }
    show(null);
  }

  function onCues() {
    if (!s || !s.cur) {
      if (s && !s.cur) { render(); }
      return;
    }
    var c = window.VP_Store.getCue(s.cur.index);
    var d = s.data && s.data.detail;
    if (c && d && d.cue && d.cue.target !== c.target) {
      s.cues.drop(s.cur.index);
      show(s.cur);
    }
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-insp-action');
    var help = btn.getAttribute('data-insp-help');
    var src = btn.getAttribute('data-insp-src');
    var choice = btn.getAttribute('data-insp-choice');
    var tgt = btn.getAttribute('data-insp-target');
    var sel = window.VP_Store.get('selection');
    if (help) {
      var fv = help.split(':');
      openHelp(fv[0], fv[1]);
    } else if (src !== null) {
      if (sel) { window.VP_Store.set('inspect', { index: sel.index, token: -1, text: src, lang: pairLangs().src, side: readingPair() ? 'analysis' : 'source' }); }
    } else if (choice !== null) {
      useCandidate(Number(choice));
    } else if (tgt !== null && s.data && s.data.detail) {
      var tok = (s.data.detail.tokens || [])[Number(tgt)];
      if (tok) { window.VP_Store.set('inspect', { index: s.cur.index, token: Number(tgt), text: tok.text, lang: pairLangs().dst, lemmaId: tok.lemmaId === undefined ? null : tok.lemmaId }); }
    } else if (a === 'forms') {
      toggleForms();
    } else if (a === 'why') {
      toggleWhy();
    } else if (a === 'another') {
      if (s.choiceEl) {
        s.choiceEl.hidden = !s.choiceEl.hidden;
        btn.setAttribute('aria-expanded', s.choiceEl.hidden ? 'false' : 'true');
        var first = window.VP_Dom.qs('[data-insp-choice]', s.choiceEl);
        if (!s.choiceEl.hidden && first) { first.focus(); }
      }
    } else if (a === 'correction') {
      addCorrection();
    } else if (a === 'back') {
      window.VP_Store.set('inspect', null);
    }
  }

  function onChange(e) {
    var t = e.target;
    if (t && t.getAttribute && t.getAttribute('data-insp-set') && s && s.grid) {
      s.verbGroup = t.value;
      var r = s.lemmas.get(s.view.lang + '|' + s.view.lemma.id);
      renderForms(r || null);
      var again = window.VP_Dom.qs('#vp-par-set', s.body);
      if (again) { again.focus(); }
    }
  }

  function mount(el2) {
    if (s) { destroy(); }
    var D = window.VP_Dom;
    s = {
      gen: 0, cur: null, data: null, view: null, others: [], formsOpen: false, verbGroup: null, grid: null, removers: [],
      cues: lru(CUE_CAP), words: lru(WORD_CAP), lemmas: lru(LEMMA_CAP), body: null, formsEl: null, whyEl: null, choiceEl: null
    };
    s.body = el('div', { className: 'vp-insp', 'aria-live': 'polite' });
    s.root = el('div', { className: 'vp-panel vp-panel-word' }, [s.body]);
    el2.appendChild(s.root);
    D.delegate(s.root, 'button', 'click', onClick, { owner: OWNER });
    D.on(s.root, 'change', onChange, { owner: OWNER });
    var S = window.VP_Store;
    s.removers.push(S.subscribe('inspect', onInspect));
    s.removers.push(S.subscribe('selection', onSelection));
    s.removers.push(S.subscribe('cues', onCues));
    s.removers.push(S.subscribe('settings', function () { render(); }));
    s.removers.push(window.VP_I18n.onLanguageChanged(function () { render(); }));
    s.removers.push(window.VP_Keys.handle('why', function (e) {
      if (window.VP_Keys.isTyping(e && e.target)) { return false; }
      toggleWhy();
    }, OWNER));
    s.removers.push(window.VP_Debug.registerCache('inspectorCueGet', function () { return s ? s.cues.size() : 0; }));
    s.removers.push(window.VP_Debug.registerCache('inspectorWords', function () { return s ? s.words.size() : 0; }));
    var x = S.get('inspect');
    var sel = S.get('selection');
    show(x && (!sel || sel.index === x.index) ? x : null);
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    s.cues.clear();
    s.words.clear();
    s.lemmas.clear();
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s = null;
  }

  function i18nKeys() {
    var keys = ['inspector.empty.title', 'inspector.loading.label', 'inspector.unknown.label', 'inspector.suggest.label', 'inspector.gloss.label', 'inspector.gloss.viaEnglish.label',
      'inspector.gloss.english.label', 'inspector.form.label', 'inspector.form.none.label', 'inspector.help.aria', 'inspector.help.abbr.label', 'inspector.help.none.label',
      'inspector.forms.caption', 'inspector.forms.used.label', 'inspector.emoji.aria', 'inspector.another.noForm.label', 'inspector.another.done.label',
      'inspector.correction.done.label', 'why.meaning.sense.label', 'why.meaning.context.label', 'why.model.label', 'why.correction.label', 'why.phrasebook.label', 'why.name.label',
      'why.reading.form.label', 'why.reading.gloss.label', 'why.reading.sure.label', 'why.reading.confidence.label', 'source.word.role.label'];
    SOURCES.forEach(function (x) { keys.push('why.evidence.source.' + x + '.label'); });
    ['yes', 'no', 'off', 'none'].forEach(function (x) { keys.push('why.evidence.state.' + x + '.label'); });
    CHECKS.forEach(function (x) { keys.push('why.check.' + x.toLowerCase() + '.label'); });
    ['common', 'rarer', 'rare'].forEach(function (x) { keys.push('why.candidate.band.' + x + '.label'); });
    ['vocabulary', 'structure'].forEach(function (x) { keys.push('why.orberg.reason.' + x + '.label'); });
    return keys;
  }

  window.VP_Inspector = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    show: show,
    toggleWhy: toggleWhy,
    toggleForms: toggleForms,
    useCandidate: useCandidate,
    addCorrection: addCorrection,
    openHelp: openHelp,
    state: function () { return s ? { word: s.cur ? s.cur.text : null, side: s.cur ? (s.cur.side || 'target') : null, loaded: !!s.data, why: whyOpen, forms: s.formsOpen, candidates: s.others.length } : null; },
    tierBadge: tierBadge,
    formWords: formWords,
    glossOf: glossOf,
    grid: grid,
    replaceToken: replaceToken,
    stats: function () { return s ? { cueGet: s.cues.size(), words: s.words.size(), lemmas: s.lemmas.size() } : null; },
    i18nKeys: i18nKeys
  };
}());
