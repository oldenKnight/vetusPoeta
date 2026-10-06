/* vp_mock_engine.js - DEV ONLY: a pretend engine for browser testing (index.html?mock=1).
 *
 * Speaks the JSON of DESIGN 9 through VP_Bridge: connect(listener) then postMessage(text);
 * answers after options.latencyMs and sends events while a fake job runs. Its cues come from
 * a small list of our own sample sentences (English, Spanish, Latin with macrons, Attic
 * Greek), picked by a seeded generator, so the same seed always gives the same project.
 * Its error hints are English like the real engine's; the UI translates by error code.
 *
 * Hooks for tests: options {latencyMs, cuesPerSecond, batch, seed, sampleCues, failNext,
 * noLexicon}, generate(n, {seed, translated, pair}), emit(eventObject), reset(), stats(),
 * sentences().
 * Paths the mock understands (B6 screens): project.new with a sourcePath inside a
 * "samples" folder (".../samples/sample.en.srt") gives the built-in 12-cue sample made of
 * the first 12 sentences in order; project.open of "...-<N>-cues..." gives N translated
 * cues (smoke test: 50,000); a path containing "recover" or "crash" answers with
 * recoverable {autosavePath, at}; "missing" -> not_found, "damaged"/"corrupt" ->
 * project_corrupt. Flags it sets on CueView.flags: "fast" (over cps.adult), "emoji" (a
 * picturable noun in the target), "unknownName" (a capitalised name it does not know).
 * Panels (B7): cue.get carries reasons for every known target word (sense, ranked candidates
 * with the chosen one first, form, evidence per source), lemma.get the paradigm cells of
 * its small lexicon (nouns, 1st/2nd declension adjectives, verbs), names.* detect "Marcus"
 * and mark the cues containing a name stale, corrections.* use the engine's shape {id, key,
 * target, scope, count}, words.list counts the lemmas of the targets, export.preview applies
 * macrons/emoji/re-break, export.write to a path containing "exists" answers io "file
 * exists" unless overwrite, model.locate accepts a ".gguf" path, orbergise.start rewrites
 * with simpler words (cue.get then adds meaning {percent, missing} and, after an
 * originalPath, original), shell.revealFile/openExternal are recorded (shellCalls()).
 * Release builds leave this file out (tools/pack_ui.py, later).
 */
(function () {
  'use strict';

  var VERSION = '0.0.0-mock';
  var SETTINGS_KEY = 'vp.mock.settings';
  // Notice blocks shaped like the lexicon files' NOTE section (our own wording, mock only).
  var NOTICE_LA = 'Mock Latin dictionary for development (not the real data).\n' +
    'Forms and meanings: Wiktionary via the Kaikki extracts, CC BY-SA 4.0.\n' +
    'Lewis & Short (Perseus), CC BY-SA 4.0.\n' +
    'DCC Latin Core Vocabulary, Dickinson College Commentaries, CC BY-SA 3.0.\n' +
    'Whitaker\'s Words data, used with the permission stated by its author.';
  var NOTICE_GRC = 'Mock Ancient Greek dictionary for development (not the real data).\n' +
    'Forms and meanings: Wiktionary via the Kaikki extracts, CC BY-SA 4.0.\n' +
    'LSJ (Perseus), CC BY-SA 4.0.\n' +
    'DCC Greek Core Vocabulary, Dickinson College Commentaries, CC BY-SA 3.0.';
  var PAIRS = ['en-la', 'es-la', 'la-en', 'la-es', 'en-grc', 'es-grc', 'grc-en', 'grc-es', 'la-la'];

  // Our own sentences (not from any book or film).
  var SENTENCES = [
    { en: 'The girl sees the rose.', es: 'La niña ve la rosa.', la: 'Puella rosam videt.', grc: 'ἡ κόρη τὸ ῥόδον ὁρᾷ.' },
    { en: 'The sailor lives on the island.', es: 'El marinero vive en la isla.', la: 'Nauta in īnsulā habitat.', grc: 'ὁ ναύτης ἐν τῇ νήσῳ οἰκεῖ.' },
    { en: 'The farmer carries water.', es: 'El agricultor lleva agua.', la: 'Agricola aquam portat.', grc: 'ὁ γεωργὸς ὕδωρ φέρει.' },
    { en: 'Marcus is reading a book.', es: 'Marco lee un libro.', la: 'Mārcus librum legit.', grc: 'ὁ Μᾶρκος βιβλίον ἀναγιγνώσκει.' },
    { en: 'The dog is sleeping in the street.', es: 'El perro duerme en la calle.', la: 'Canis in viā dormit.', grc: 'ὁ κύων ἐν τῇ ὁδῷ καθεύδει.' },
    { en: 'The boys are playing in the garden.', es: 'Los niños juegan en el jardín.', la: 'Puerī in hortō lūdunt.', grc: 'οἱ παῖδες ἐν τῷ κήπῳ παίζουσιν.' },
    { en: 'The mother calls her daughter.', es: 'La madre llama a su hija.', la: 'Māter fīliam vocat.', grc: 'ἡ μήτηρ τὴν θυγατέρα καλεῖ.' },
    { en: 'The sun shines in the sky.', es: 'El sol brilla en el cielo.', la: 'Sōl in caelō lūcet.', grc: 'ὁ ἥλιος ἐν τῷ οὐρανῷ λάμπει.' },
    { en: 'The wolf runs through the forest.', es: 'El lobo corre por el bosque.', la: 'Lupus per silvam currit.', grc: 'ὁ λύκος διὰ τῆς ὕλης τρέχει.' },
    { en: 'The students listen to the teacher.', es: 'Los estudiantes escuchan al maestro.', la: 'Discipulī magistrum audiunt.', grc: 'οἱ μαθηταὶ τοῦ διδασκάλου ἀκούουσιν.' },
    { en: 'The ship comes to the harbour.', es: 'El barco llega al puerto.', la: 'Nāvis ad portum venit.', grc: 'τὸ πλοῖον εἰς τὸν λιμένα ἔρχεται.' },
    { en: 'The bird sings in the tree.', es: 'El pájaro canta en el árbol.', la: 'Avis in arbore cantat.', grc: 'ἡ ὄρνις ἐν τῷ δένδρῳ ᾄδει.' },
    { en: 'How are you, friend?', es: '¿Cómo estás, amigo?', la: 'Quid agis, amīce?', grc: 'πῶς ἔχεις, ὦ φίλε;' },
    { en: 'I am well, thank you.', es: 'Estoy bien, gracias.', la: 'Bene valeō, grātiās tibi agō.', grc: 'καλῶς ἔχω, χάριν σοι ἔχω.' },
    { en: 'Today the sky is clear.', es: 'Hoy el cielo está despejado.', la: 'Hodiē caelum serēnum est.', grc: 'σήμερον ὁ οὐρανὸς αἴθριός ἐστιν.' },
    { en: 'The queen gives the girl a gift.', es: 'La reina da un regalo a la niña.', la: 'Rēgīna puellae dōnum dat.', grc: 'ἡ βασίλεια τῇ κόρῃ δῶρον δίδωσιν.' }
  ];

  // A tiny lexicon for word.inspect, lemma.get, words.list and the "why this word" reasons:
  // id -> LemmaView fields plus what the paradigm generator needs (stem, nom, gpl, conj).
  var LEMMAS = {
    puella: { head: 'puella, puellae', pos: 'noun', gender: 'feminine', cls: '1', tier: 1, glossEn: 'girl', glossEs: 'niña', emoji: '👧', stem: 'puell' },
    rosa: { head: 'rosa, rosae', pos: 'noun', gender: 'feminine', cls: '1', tier: 1, glossEn: 'rose', glossEs: 'rosa', emoji: '🌹', stem: 'ros' },
    video: { head: 'videō, vidēre, vīdī, vīsum', pos: 'verb', cls: '2', tier: 1, glossEn: 'see', glossEs: 'ver', emoji: '', stem: 'vid', perf: 'vīd' },
    nauta: { head: 'nauta, nautae', pos: 'noun', gender: 'masculine', cls: '1', tier: 1, glossEn: 'sailor', glossEs: 'marinero', emoji: '', stem: 'naut' },
    insula: { head: 'īnsula, īnsulae', pos: 'noun', gender: 'feminine', cls: '1', tier: 1, glossEn: 'island', glossEs: 'isla', emoji: '🏝️', stem: 'īnsul' },
    canis: { head: 'canis, canis', pos: 'noun', gender: 'masculine', cls: '3', tier: 1, glossEn: 'dog', glossEs: 'perro', emoji: '🐕', stem: 'can', nom: 'canis', gpl: 'canum' },
    lupus: { head: 'lupus, lupī', pos: 'noun', gender: 'masculine', cls: '2', tier: 1, glossEn: 'wolf', glossEs: 'lobo', emoji: '🐺', stem: 'lup' },
    sol: { head: 'sōl, sōlis', pos: 'noun', gender: 'masculine', cls: '3', tier: 1, glossEn: 'sun', glossEs: 'sol', emoji: '☀️', stem: 'sōl', nom: 'sōl' },
    navis: { head: 'nāvis, nāvis', pos: 'noun', gender: 'feminine', cls: '3', tier: 1, glossEn: 'ship', glossEs: 'barco', emoji: '⛵', stem: 'nāv', nom: 'nāvis', gpl: 'nāvium' },
    avis: { head: 'avis, avis', pos: 'noun', gender: 'feminine', cls: '3', tier: 2, glossEn: 'bird', glossEs: 'pájaro', emoji: '🐦', stem: 'av', nom: 'avis', gpl: 'avium' },
    serenus: { head: 'serēnus, serēna, serēnum', pos: 'adj', cls: '1-2', tier: 3, glossEn: 'clear, calm', glossEs: 'despejado, sereno', emoji: '', stem: 'serēn', pivot: true },
    virgo: { head: 'virgō, virginis', pos: 'noun', gender: 'feminine', cls: '3', tier: 2, glossEn: 'young woman, maiden', glossEs: 'doncella', emoji: '', stem: 'virgin', nom: 'virgō' },
    specto: { head: 'spectō, spectāre, spectāvī, spectātum', pos: 'verb', cls: '1', tier: 1, glossEn: 'look at, watch', glossEs: 'mirar', emoji: '', stem: 'spect', perf: 'spectāv' },
    cerno: { head: 'cernō, cernere, crēvī, crētum', pos: 'verb', cls: '3', tier: 3, glossEn: 'perceive, discern', glossEs: 'distinguir', emoji: '', stem: 'cern', perf: 'crēv' },
    clarus: { head: 'clārus, clāra, clārum', pos: 'adj', cls: '1-2', tier: 1, glossEn: 'bright, clear', glossEs: 'claro', emoji: '', stem: 'clār' },
    vivo: { head: 'vīvō, vīvere, vīxī, vīctum', pos: 'verb', cls: '3', tier: 1, glossEn: 'live', glossEs: 'vivir', emoji: '', stem: 'vīv', perf: 'vīx' },
    habito: { head: 'habitō, habitāre, habitāvī, habitātum', pos: 'verb', cls: '1', tier: 2, glossEn: 'live, dwell', glossEs: 'habitar', emoji: '', stem: 'habit', perf: 'habitāv' }
  };
  // Other lemmas the mock offers as candidates for "Use another word" (the first one is chosen).
  var ALTS = { puella: ['virgo'], video: ['specto', 'cerno'], serenus: ['clarus'], habito: ['vivo'] };
  // Orbergise swaps: form -> [new form, lemma id, why, loose (meaning not fully kept)].
  var SWAPS = { 'serēnum': ['clārum', 'clarus', 'vocabulary', false], habitat: ['vīvit', 'vivo', 'vocabulary', true], videt: ['spectat', 'specto', 'vocabulary', false] };
  // form -> [lemma id, case, number, person, tense, gender (adjectives)]
  var FORMS = {
    puella: ['puella', 'nominative', 'singular'], puellae: ['puella', 'dative', 'singular'],
    rosam: ['rosa', 'accusative', 'singular'], videt: ['video', '', 'singular', 'third', 'present'],
    nauta: ['nauta', 'nominative', 'singular'], 'īnsulā': ['insula', 'ablative', 'singular'],
    canis: ['canis', 'nominative', 'singular'], lupus: ['lupus', 'nominative', 'singular'],
    'sōl': ['sol', 'nominative', 'singular'], 'nāvis': ['navis', 'nominative', 'singular'],
    avis: ['avis', 'nominative', 'singular'], 'serēnum': ['serenus', 'nominative', 'singular', '', '', 'neuter'],
    habitat: ['habito', '', 'singular', 'third', 'present'], spectat: ['specto', '', 'singular', 'third', 'present'],
    'clārum': ['clarus', 'nominative', 'singular', '', '', 'neuter'], 'vīvit': ['vivo', '', 'singular', 'third', 'present']
  };
  var CASES = ['nominative', 'vocative', 'accusative', 'genitive', 'dative', 'ablative'];
  var PERSONS = [['first', 'singular'], ['second', 'singular'], ['third', 'singular'], ['first', 'plural'], ['second', 'plural'], ['third', 'plural']];
  // Endings in CASES order, singular then plural ("*" = the nominative as written).
  var NOUN_END = {
    '1': ['a', 'a', 'am', 'ae', 'ae', 'ā', 'ae', 'ae', 'ās', 'ārum', 'īs', 'īs'],
    '2': ['us', 'e', 'um', 'ī', 'ō', 'ō', 'ī', 'ī', 'ōs', 'ōrum', 'īs', 'īs'],
    '2n': ['um', 'um', 'um', 'ī', 'ō', 'ō', 'a', 'a', 'a', 'ōrum', 'īs', 'īs'],
    '3': ['*', '*', 'em', 'is', 'ī', 'e', 'ēs', 'ēs', 'ēs', 'um', 'ibus', 'ibus']
  };
  // Verb endings per conjugation: [present, imperfect, future, present subjunctive, present passive]
  // on the bare stem, perfect and pluperfect on the perfect stem.
  var VERB_END = {
    '1': [['ō', 'ās', 'at', 'āmus', 'ātis', 'ant'], ['ābam', 'ābās', 'ābat', 'ābāmus', 'ābātis', 'ābant'], ['ābō', 'ābis', 'ābit', 'ābimus', 'ābitis', 'ābunt'], ['em', 'ēs', 'et', 'ēmus', 'ētis', 'ent'], ['or', 'āris', 'ātur', 'āmur', 'āminī', 'antur'], 'āre'],
    '2': [['eō', 'ēs', 'et', 'ēmus', 'ētis', 'ent'], ['ēbam', 'ēbās', 'ēbat', 'ēbāmus', 'ēbātis', 'ēbant'], ['ēbō', 'ēbis', 'ēbit', 'ēbimus', 'ēbitis', 'ēbunt'], ['eam', 'eās', 'eat', 'eāmus', 'eātis', 'eant'], ['eor', 'ēris', 'ētur', 'ēmur', 'ēminī', 'entur'], 'ēre'],
    '3': [['ō', 'is', 'it', 'imus', 'itis', 'unt'], ['ēbam', 'ēbās', 'ēbat', 'ēbāmus', 'ēbātis', 'ēbant'], ['am', 'ēs', 'et', 'ēmus', 'ētis', 'ent'], ['am', 'ās', 'at', 'āmus', 'ātis', 'ant'], ['or', 'eris', 'itur', 'imur', 'iminī', 'untur'], 'ere']
  };
  var PERFECT = [['ī', 'istī', 'it', 'imus', 'istis', 'ērunt'], ['eram', 'erās', 'erat', 'erāmus', 'erātis', 'erant']];
  var MACRON_PLAIN = { 'ā': 'a', 'ē': 'e', 'ī': 'i', 'ō': 'o', 'ū': 'u', 'ȳ': 'y', 'Ā': 'A', 'Ē': 'E', 'Ī': 'I', 'Ō': 'O', 'Ū': 'U', 'Ȳ': 'Y' };
  var WORD_RE = /[^\s.,;:?!¿¡"“”«»()\[\]{}\-–—]+/g;
  var known = null;

  // Every word of the sample sentences counts as known (so the editor only underlines
  // words the mock has never seen); FORMS adds lemma, gloss and features for a few.
  function knownForms() {
    if (known) { return known; }
    known = {};
    for (var i = 0; i < SENTENCES.length; i++) {
      var words = (SENTENCES[i].la + ' ' + SENTENCES[i].grc).match(WORD_RE) || [];
      for (var w = 0; w < words.length; w++) { known[plainKey(words[w])] = words[w]; }
    }
    return known;
  }

  function plainKey(word) {
    return String(word).toLowerCase().replace(/[āēīōūȳĀĒĪŌŪȲ]/g, function (c) { return MACRON_PLAIN[c].toLowerCase(); });
  }

  var options = {
    latencyMs: 15,
    cuesPerSecond: 200,
    batch: 20,
    seed: 7,
    sampleCues: 12,
    failNext: null,
    noLexicon: false,
    autosaveMs: 1000
  };

  var listener = null;
  var state = null;
  var counts = { requests: 0, events: 0 };

  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  // Park-Miller generator: exact in doubles, deterministic for a seed.
  function rng(seed) {
    var s = (Math.abs(Math.floor(seed)) % 2147483646) + 1;
    return function () {
      s = (s * 16807) % 2147483647;
      return (s - 1) / 2147483646;
    };
  }

  function fail(code, message, hint) {
    var e = new Error(message);
    e.engineError = { code: code, message: message, hint: hint || '' };
    return e;
  }

  function defaultSettings() {
    return {
      lang: '', theme: 'auto', textScale: 100, showMacrons: true, showEmoji: true, grammarColours: false,
      defaultPair: 'en-la', defaultFidelity: 2,
      'export': { emoji: false, macrons: false, encoding: 'utf-8', bom: false, rebreak: true },
      engines: { model: false, online: false },
      online: { wiktionary: false, latinitium: false },
      modelPath: '', eco: false, autosave: true, cps: { adult: 17, child: 20 },
      tourSeenVersion: '', recentProjects: []
    };
  }

  function loadSettings() {
    var s = defaultSettings();
    try {
      var raw = window.localStorage.getItem(SETTINGS_KEY);
      if (raw) { merge(s, JSON.parse(raw)); }
    } catch (e) { /* private mode or bad JSON: defaults */ }
    return s;
  }

  function saveSettings() {
    try { window.localStorage.setItem(SETTINGS_KEY, JSON.stringify(state.settings)); } catch (e) { /* ignore */ }
  }

  function merge(target, patch) {
    var keys = Object.keys(patch || {});
    for (var i = 0; i < keys.length; i++) {
      var k = keys[i];
      var v = patch[k];
      if (v && typeof v === 'object' && Object.prototype.toString.call(v) !== '[object Array]' && target[k] && typeof target[k] === 'object') {
        merge(target[k], v);
      } else {
        target[k] = v;
      }
    }
    return target;
  }

  function reset() {
    if (state && state.job) { stopJob(); }
    stopAutosave();
    state = { settings: loadSettings(), project: null, cues: [], undo: [], redo: [], corrections: [], nextCorrection: 1, names: [], job: null, nextJob: 1, originalPath: '',
      model: { available: false, path: '', sizeBytes: 0, loaded: false, lastLoadMs: 0 }, shell: [] };
    counts.requests = 0;
    counts.events = 0;
  }

  function pad(n, width) {
    var s = String(n);
    while (s.length < width) { s = '0' + s; }
    return s;
  }

  function timing(ms) {
    var h = Math.floor(ms / 3600000);
    var m = Math.floor(ms / 60000) % 60;
    var s = Math.floor(ms / 1000) % 60;
    return pad(h, 2) + ':' + pad(m, 2) + ':' + pad(s, 2) + ',' + pad(ms % 1000, 3);
  }

  function langsOf(pair) {
    var parts = String(pair || 'en-la').split('-');
    return { src: parts[0], dst: parts[1] };
  }

  // Lines as a player shows them: one line up to 42 characters, else two balanced lines
  // broken at the space nearest the middle (the real engine follows D15 more closely).
  function breakTwo(text) {
    if (text.length <= 42) { return [text]; }
    var mid = Math.floor(text.length / 2);
    var best = -1;
    for (var i = 0; i < text.length; i++) {
      if (text.charAt(i) === ' ' && (best < 0 || Math.abs(i - mid) < Math.abs(best - mid))) { best = i; }
    }
    return best < 0 ? [text] : [text.slice(0, best), text.slice(best + 1)];
  }

  function confidenceOf(r) {
    if (r < 0.7) { return 'ok'; }
    return r < 0.9 ? 'check' : 'fix';
  }

  function translateCue(cue, random) {
    var s = SENTENCES[cue.sentence];
    var dst = langsOf(cue.pair).dst;
    var r = random();
    cue.target = s[dst];
    cue.state = 'translated';
    cue.confidence = confidenceOf(r);
    cue.score = Math.round((1 - r) * 100) / 100;
    cue.lines = breakTwo(cue.target);
    cue.cps = cue.durationMs ? Math.round(cue.target.length / (cue.durationMs / 1000) * 10) / 10 : 0;
    cue.flags = cue.cps > state.settings.cps.adult ? ['cps'] : [];
    if (cue.lines.length > 2 || cue.lines.some(function (l) { return l.length > 42; })) { cue.flags.push('overflow'); }
    var words = cue.target.match(WORD_RE) || [];
    for (var i = 0; i < words.length; i++) {
      var f = FORMS[words[i].toLowerCase()];
      if (f && LEMMAS[f[0]].emoji) {
        cue.flags.push('emoji');
        break;
      }
    }
    if (/Marc/.test(cue.source)) { cue.flags.push('unknownName'); }
    return cue;
  }

  // n synthetic cues; opts {seed, translated, pair}
  function generate(n, opts) {
    opts = opts || {};
    var random = rng(opts.seed === undefined ? options.seed : opts.seed);
    var pair = opts.pair || 'en-la';
    var src = langsOf(pair).src;
    var out = [];
    var t = 1000;
    for (var i = 0; i < n; i++) {
      var sentence = Math.floor(random() * SENTENCES.length);
      var dur = 1500 + Math.floor(random() * 2500);
      var cue = {
        index: i, idRaw: String(i + 1), timingRaw: timing(t) + ' --> ' + timing(t + dur),
        start: t, end: t + dur, durationMs: dur, source: SENTENCES[sentence][src], target: '',
        state: 'new', confidence: 'check', score: 0, cps: 0, lines: [], flags: [],
        sentence: sentence, pair: pair
      };
      if (opts.translated) { translateCue(cue, random); }
      out.push(cue);
      t += dur + 200 + Math.floor(random() * 800);
    }
    return out;
  }

  function view(cue) {
    var v = clone(cue);
    delete v.sentence;
    delete v.pair;
    delete v.prevState;
    delete v.swaps;
    delete v.meaning;
    return v;
  }

  function stats() {
    var p = { cues: state.cues.length, translated: 0, reviewed: 0, needReview: 0, check: 0, fix: 0 };
    for (var i = 0; i < state.cues.length; i++) {
      var c = state.cues[i];
      if (c.state !== 'new') { p.translated++; }
      if (c.state === 'reviewed') { p.reviewed++; }
      if ((c.state === 'translated' || c.state === 'stale') && c.confidence !== 'ok') {
        p.needReview++;
        p[c.confidence]++;
      }
    }
    return p;
  }

  function manifest() {
    var p = state.project;
    var s = stats();
    var news = s.cues - s.translated;
    // The real engine's shape (engine/cli/README.md: path, autosavePath, manifest, stats,
    // canUndo, canRedo) plus the flat fields older tests read.
    return {
      name: p.name, path: p.path, kind: p.kind, pair: p.pair, cues: s.cues, translated: s.translated, reviewed: s.reviewed, needReview: s.needReview, check: s.check, fix: s.fix,
      autosavePath: (p.path || 'mock://data/unsaved/untitled.vpoeta') + '.autosave',
      manifest: { pair: p.pair, kind: p.kind, sourceFileName: p.name },
      stats: { total: s.cues, 'new': news, translated: s.translated, reviewed: s.reviewed, check: s.check, fix: s.fix },
      dirty: false, canUndo: state.undo.length > 0, canRedo: state.redo.length > 0
    };
  }

  function needProject() {
    if (!state.project) { throw fail('bad_params', 'no project is open', 'Open or create a project first.'); }
  }

  function cueAt(index) {
    needProject();
    var c = state.cues[index];
    if (!c) { throw fail('not_found', 'no cue ' + index, 'The cue list changed; reload it.'); }
    return c;
  }

  function newProject(params) {
    if (PAIRS.indexOf(params.pair) < 0) { throw fail('bad_params', 'unknown pair ' + params.pair, 'Choose a language pair from the list.'); }
    var kind = params.kind === 'text' ? 'text' : 'subs';
    var cues;
    if (kind === 'text') {
      var paras = String(params.text || '').split(/\n\s*\n/).filter(function (p) { return p.replace(/\s+/g, ''); });
      if (!paras.length) { throw fail('bad_params', 'empty text', 'Type or paste some text first.'); }
      cues = paras.map(function (text, i) {
        return { index: i, idRaw: String(i + 1), timingRaw: '', start: 0, end: 0, durationMs: 0, source: text.replace(/^\s+|\s+$/g, ''), target: '', state: 'new', confidence: 'check', score: 0, cps: 0, lines: [], flags: [], sentence: i % SENTENCES.length, pair: params.pair };
      });
    } else if (/[\\\/]samples[\\\/]sample\./i.test(String(params.sourcePath || ''))) {
      cues = sampleCues(params.pair);
    } else {
      cues = generate(params.count || options.sampleCues, { pair: params.pair, seed: options.seed });
    }
    var path = params.sourcePath || '';
    state.project = { name: path ? String(path).split(/[\\\/]/).pop() : 'sample.srt', path: null, kind: kind, pair: params.pair };
    state.cues = cues;
    state.undo = [];
    state.redo = [];
    state.names = [];
    state.corrections = [];
    state.originalPath = '';
    return { project: manifest() };
  }

  // The built-in sample: the first 12 sentences in order, 2.5 s apart (our own sentences).
  function sampleCues(pair) {
    var src = langsOf(pair).src;
    var out = [];
    for (var i = 0; i < 12; i++) {
      var t = 1000 + i * 3000;
      out.push({
        index: i, idRaw: String(i + 1), timingRaw: timing(t) + ' --> ' + timing(t + 2500),
        start: t, end: t + 2500, durationMs: 2500, source: SENTENCES[i][src], target: '',
        state: 'new', confidence: 'check', score: 0, cps: 0, lines: [], flags: [],
        sentence: i, pair: pair
      });
    }
    return out;
  }

  function openProject(params) {
    var path = String(params.path || '');
    if (!path) { throw fail('bad_params', 'path is required', 'Choose a file.'); }
    if (/missing/i.test(path)) { throw fail('not_found', 'file not found', 'The file was moved or deleted.'); }
    if (/damaged|corrupt/i.test(path)) { throw fail('project_corrupt', 'project file is damaged', 'Open the last autosave instead.'); }
    var big = /(\d+)-cues/i.exec(path);
    var i;
    if (big) {
      var n = Math.max(1, Math.min(200000, Number(big[1])));
      newProject({ pair: 'en-la', count: n });
      var r = rng(options.seed + 2);
      for (i = 0; i < n; i++) { translateCue(state.cues[i], r); }
    } else {
      newProject({ pair: 'en-la', count: 40 });
      var random = rng(options.seed + 1);
      for (i = 0; i < 24; i++) { translateCue(state.cues[i], random); }
    }
    state.project.path = path;
    state.project.name = path.split(/[\\\/]/).pop();
    addRecent(path);
    var out = { project: manifest(), warnings: [] };
    if (/recover|crash/i.test(path)) {
      out.recoverable = { autosavePath: path + '.autosave', at: new Date(Date.now() - 8 * 60000).toISOString() };
    }
    return out;
  }

  function alternativesOf(cue) {
    if (!cue.target) { return []; }
    var words = cue.target.split(' ');
    var swapped = words.length > 2 ? [words[1], words[0]].concat(words.slice(2)).join(' ') : cue.target;
    return [
      { text: cue.target, reason: 'best', score: cue.score },
      { text: swapped, reason: 'order', score: Math.round(cue.score * 80) / 100 },
      { text: cue.target.replace(/[āēīōūȳĀĒĪŌŪȲ]/g, function (c) { return MACRON_PLAIN[c]; }), reason: 'plain', score: Math.round(cue.score * 60) / 100 }
    ];
  }

  function tokensOf(text) {
    var out = [];
    var re = new RegExp(WORD_RE.source, 'g');
    var forms = knownForms();
    var m;
    while ((m = re.exec(text)) !== null) {
      var form = m[0].toLowerCase();
      var f = FORMS[form];
      var tok = { text: m[0], display: m[0], start: m.index, end: m.index + m[0].length };
      if (f) {
        tok.lemmaId = f[0];
        tok.features = featureView(f);
        tok.tier = LEMMAS[f[0]].tier;
        if (LEMMAS[f[0]].emoji) { tok.emoji = LEMMAS[f[0]].emoji; }
      } else {
        tok.unknown = !forms[plainKey(m[0])];
      }
      out.push(tok);
    }
    return out;
  }

  function lemmaView(id) {
    var l = LEMMAS[id];
    var flags = l.pos === 'noun' || l.pos === 'adj' || l.pos === 'verb' ? ['has-table'] : [];
    if (l.pivot) { flags.push('gloss-es-pivot'); }
    return { id: id, head: l.head, pos: l.pos, gender: l.gender || '', cls: l.cls, tier: l.tier, tierSource: 'mock', freqRank: 0, whitFreq: '', glossEn: l.glossEn, glossEs: l.glossEs, emoji: l.emoji, principal: l.head, flags: flags };
  }

  function featureView(f) {
    var l = LEMMAS[f[0]];
    var verb = l.pos === 'verb';
    return { pos: l.pos, 'case': f[1] || '', number: f[2] || '', gender: f[5] || (verb ? '' : l.gender || ''), person: f[3] || '', tense: f[4] || '', mood: verb ? 'indicative' : '', voice: verb ? 'active' : '', degree: l.pos === 'adj' ? 'positive' : '' };
  }

  function feats(o) {
    return { pos: o.pos || '', 'case': o['case'] || '', number: o.number || '', gender: o.gender || '', person: o.person || '', tense: o.tense || '', mood: o.mood || '', voice: o.voice || '', degree: o.degree || '' };
  }

  function nounCells(l, pos, gender, table) {
    var out = [];
    var end = NOUN_END[table];
    for (var i = 0; i < 12; i++) {
      var e = end[i];
      out.push({ features: feats({ pos: pos, 'case': CASES[i % 6], number: i < 6 ? 'singular' : 'plural', gender: gender, degree: pos === 'adj' ? 'positive' : '' }), form: e === '*' ? l.nom : l.stem + (i === 9 && l.gpl ? l.gpl.slice(l.stem.length) : e) });
    }
    return out;
  }

  // The paradigm of a mock lemma (lemma.get cells): nouns by declension, 1st/2nd declension
  // adjectives in three genders, verbs by conjugation (finite active and passive, infinitives).
  function cellsOf(id) {
    var l = LEMMAS[id];
    if (l.pos === 'noun') { return nounCells(l, 'noun', l.gender, l.cls === '1' ? '1' : (l.cls === '2' ? '2' : '3')); }
    if (l.pos === 'adj') {
      return nounCells(l, 'adj', 'masculine', '2').concat(nounCells(l, 'adj', 'feminine', '1'), nounCells(l, 'adj', 'neuter', '2n'));
    }
    var out = [];
    var v = VERB_END[l.cls];
    var sets = [['present', 'indicative', 'active', v[0], l.stem], ['imperfect', 'indicative', 'active', v[1], l.stem], ['future', 'indicative', 'active', v[2], l.stem],
      ['perfect', 'indicative', 'active', PERFECT[0], l.perf], ['pluperfect', 'indicative', 'active', PERFECT[1], l.perf],
      ['present', 'subjunctive', 'active', v[3], l.stem], ['present', 'indicative', 'passive', v[4], l.stem]];
    for (var s = 0; s < sets.length; s++) {
      for (var k = 0; k < 6; k++) {
        out.push({ features: feats({ pos: 'verb', person: PERSONS[k][0], number: PERSONS[k][1], tense: sets[s][0], mood: sets[s][1], voice: sets[s][2] }), form: sets[s][4] + sets[s][3][k] });
      }
    }
    out.push({ features: feats({ pos: 'verb', tense: 'present', mood: 'infinitive', voice: 'active' }), form: l.stem + v[5] });
    out.push({ features: feats({ pos: 'verb', tense: 'perfect', mood: 'infinitive', voice: 'active' }), form: l.perf + 'isse' });
    return out;
  }

  // The form of lemma `id` with the features of `f` (FeatureView), or ''.
  function formFor(id, f) {
    var cells = cellsOf(id);
    for (var i = 0; i < cells.length; i++) {
      var c = cells[i].features;
      if (c['case'] === (f['case'] || '') && c.number === (f.number || '') && c.person === (f.person || '') && c.tense === (f.tense || '') &&
        (c.mood || '') === (f.mood || '') && (!c.gender || !f.gender || c.gender === f.gender || LEMMAS[id].pos === 'noun')) { return cells[i].form; }
    }
    return '';
  }

  function srcWord(cue, id) {
    var l = LEMMAS[id];
    var gloss = langsOf(cue.pair).src === 'es' ? l.glossEs : l.glossEn;
    var words = String(cue.source).toLowerCase();
    var cands = gloss.split(/,\s*/);
    for (var i = 0; i < cands.length; i++) { if (words.indexOf(cands[i].split(' ')[0]) >= 0) { return cands[i]; } }
    return cands[0];
  }

  // "Why this word" for every known target token: sense, ranked candidates (the chosen one
  // first), form, and evidence per source (model and online "off" unless switched on).
  function reasonsOf(cue) {
    var out = [];
    var toks = tokensOf(cue.target || '');
    var words = toks.map(function (t) { return t.text; });
    for (var k = 0; k < toks.length; k++) {
      var t = toks[k];
      if (!t.lemmaId) { continue; }
      var id = t.lemmaId;
      var l = LEMMAS[id];
      var orb = cue.swaps && cue.swaps[k];
      if (orb) {
        out.push({ tokenIndex: k, kind: 'candidate', text: 'Simpler word: ' + orb.was + ' -> ' + orb.now, data: { was: orb.was, now: orb.now, why: orb.why, lemmaId: id, head: l.head, tier: l.tier, chosen: true } });
      }
      var src = srcWord(cue, id);
      out.push({ tokenIndex: k, kind: 'sense', text: 'Source word "' + src + '": ' + l.glossEn, data: { source: src, sense: l.glossEn, context: words.filter(function (w, i) { return i !== k; }).slice(0, 2) } });
      var ids = [id].concat(ALTS[id] || []);
      for (var a = 0; a < ids.length; a++) {
        var al = LEMMAS[ids[a]];
        var band = al.tier === 1 ? 'common' : (al.tier === 2 ? 'rarer' : 'rare');
        out.push({ tokenIndex: k, kind: 'candidate', text: a === 0 ? 'best match for the sense' : (al.tier > l.tier ? 'rarer word' : 'other shade of meaning'),
          data: { lemmaId: ids[a], head: al.head, form: a === 0 ? t.text : formFor(ids[a], t.features), tier: al.tier, band: band, chosen: a === 0, gloss: al.glossEn } });
      }
      out.push({ tokenIndex: k, kind: 'form', text: 'form chosen by the grammar rules', data: { features: t.features } });
      var st = state.settings.engines || {};
      out.push({ tokenIndex: k, kind: 'evidence', text: '', data: { source: 'wiktionary', state: 'yes' } });
      out.push({ tokenIndex: k, kind: 'evidence', text: '', data: { source: 'whitaker', state: l.tier === 3 ? 'no' : 'yes' } });
      out.push({ tokenIndex: k, kind: 'evidence', text: '', data: { source: 'model', state: st.model ? 'yes' : 'off' } });
      out.push({ tokenIndex: k, kind: 'evidence', text: '', data: { source: 'online', state: st.online ? 'yes' : 'off' } });
    }
    return out;
  }

  function checksOf(cue) {
    var out = [];
    for (var i = 1; i <= 9; i++) { out.push({ id: 'A' + i, ok: cue.confidence === 'ok' || i !== 3, detail: '' }); }
    return out;
  }

  // Orbergise one cue: simpler words from SWAPS (tier ceiling respected), reasons per change.
  function orbergCue(cue, opts) {
    var words = String(cue.source).split(' ');
    cue.swaps = {};
    var missing = [];
    var content = 0;
    var toks = 0;
    for (var i = 0; i < words.length; i++) {
      var bare = words[i].replace(/[.,;:?!]+$/, '');
      var sw = SWAPS[bare.toLowerCase()];
      if (FORMS[bare.toLowerCase()]) { content++; }
      if (sw && LEMMAS[sw[1]].tier <= opts.tier && (bare.toLowerCase() !== 'videt' || opts.simplify)) {
        words[i] = sw[0] + words[i].slice(bare.length);
        cue.swaps[toks] = { was: bare, now: sw[0], why: sw[2] };
        if (sw[3]) { missing.push(LEMMAS[FORMS[bare.toLowerCase()][0]].head.split(',')[0]); }
      }
      if (bare) { toks++; }
    }
    cue.target = words.join(' ');
    cue.state = 'translated';
    cue.confidence = missing.length ? 'check' : 'ok';
    cue.score = missing.length ? 0.7 : 0.95;
    cue.lines = breakTwo(cue.target);
    cue.flags = [];
    cue.meaning = { percent: content ? Math.round(100 * (content - missing.length) / content) : 100, missing: missing };
    return cue;
  }

  function job() { return state.job; }

  function modelStatus() {
    var m = state.model;
    return { available: m.available, path: m.path, sizeBytes: m.sizeBytes, sha256ok: m.available, loaded: m.loaded, cpuOk: true, lastLoadMs: m.lastLoadMs };
  }

  // Detected names (capitalised words of a small table) merged with the policies set so far.
  var KNOWN_NAMES = { Marcus: ['Mārcus', 'masculine', '2'], Marco: ['Mārcus', 'masculine', '2'], 'Μᾶρκος': ['Mārcus', 'masculine', '2'] };
  function namesList() {
    var out = [];
    var seen = {};
    function count(name) {
      var n = 0;
      state.cues.forEach(function (c) { n += (String(c.source).split(name).length - 1); });
      return n;
    }
    state.cues.forEach(function (c) {
      Object.keys(KNOWN_NAMES).forEach(function (name) {
        if (!seen[name] && String(c.source).indexOf(name) >= 0) {
          seen[name] = true;
          out.push({ name: name, policy: 'decline', form: KNOWN_NAMES[name][0], forms: [], gender: KNOWN_NAMES[name][1], declension: KNOWN_NAMES[name][2] });
        }
      });
    });
    state.names.forEach(function (e) {
      out = out.filter(function (n) { return n.name !== e.name; });
      out.push({ name: e.name, policy: e.policy, form: e.form, forms: [], gender: e.gender, declension: e.declension });
    });
    out.forEach(function (n) { n.count = count(n.name); });
    return out;
  }

  // Vocabulary of the translated text: lemma counts, tier share over the known words.
  function wordsList() {
    var counts = {};
    var known = 0;
    var unknown = 0;
    var tiers = { t1: 0, t2: 0, t3: 0, names: 0 };
    state.cues.forEach(function (c) {
      tokensOf(c.target || '').forEach(function (t) {
        if (/^M[āa]rc/.test(t.text)) {
          known++;
          tiers.names++;
          return;
        }
        if (!t.lemmaId) {
          unknown++;
          return;
        }
        known++;
        counts[t.lemmaId] = (counts[t.lemmaId] || 0) + 1;
        tiers['t' + LEMMAS[t.lemmaId].tier]++;
      });
    });
    var words = Object.keys(counts).sort(function (a, b) { return counts[b] - counts[a] || (a < b ? -1 : 1); }).map(function (id) {
      return { lemma: lemmaView(id), count: counts[id], tier: LEMMAS[id].tier };
    });
    var share = function (n) { return known ? Math.round(1000 * n / known) / 1000 : 0; };
    return { words: words, tierShare: { t1: share(tiers.t1), t2: share(tiers.t2), t3: share(tiers.t3), names: share(tiers.names) }, tokens: known + unknown, unknown: unknown };
  }

  // Like the engine: the paths of the last 20 projects, newest first, kept on open/save.
  function addRecent(path) {
    var list = [path].concat((state.settings.recentProjects || []).filter(function (x) { return typeof x === 'string' && x !== path; }));
    state.settings.recentProjects = list.slice(0, 20);
    saveSettings();
  }

  // Like the engine: project.autosaved {path, at} a moment after the last change.
  function noteChange() {
    if (!state.project) { return; }
    if (state.autosaveTimer) { window.clearTimeout(state.autosaveTimer); }
    state.autosaveTimer = window.setTimeout(function () {
      state.autosaveTimer = null;
      if (state.project) { emit({ event: 'project.autosaved', path: manifest().autosavePath, at: new Date().toISOString() }); }
    }, options.autosaveMs);
  }

  function stopAutosave() {
    if (state && state.autosaveTimer) { window.clearTimeout(state.autosaveTimer); }
    if (state) { state.autosaveTimer = null; }
  }

  function snapshot(c) {
    return { index: c.index, target: c.target, state: c.state, lines: c.lines.slice(), prevState: c.prevState };
  }

  // One undo step holds the before-snapshots of every cue it touched.
  function pushUndo(list) {
    state.undo.push(list.map(snapshot));
    state.redo = [];
  }

  function restore(entry) {
    var back = [];
    for (var i = 0; i < entry.length; i++) {
      var c = state.cues[entry[i].index];
      back.push(snapshot(c));
      c.target = entry[i].target;
      c.state = entry[i].state;
      c.lines = entry[i].lines;
      c.prevState = entry[i].prevState;
    }
    return back;
  }

  function stopJob() {
    if (state.job && state.job.timer !== null) { window.clearTimeout(state.job.timer); }
    state.job = null;
  }

  function startTranslate(params, orberg) {
    needProject();
    if (state.job) { throw fail('busy', 'a job is running', 'Wait for it to finish or cancel it.'); }
    // Without indices: every cue except the ones the user reviewed or edited (they are
    // locked from bulk re-translation, PREDESIGN 4.1).
    var indices = params.indices || state.cues.filter(function (c) { return c.state !== 'reviewed' && c.state !== 'edited'; }).map(function (c) { return c.index; });
    var id = 'job' + (state.nextJob++);
    var random = rng(options.seed + state.nextJob);
    var started = now();
    var jb = { id: id, indices: indices.slice(), done: 0, timer: null, random: random, stats: { ok: 0, check: 0, fix: 0 }, orberg: orberg ? { tier: params.tier === 1 ? 1 : 2, simplify: params.simplify !== false } : null };
    if (orberg && params.originalPath) { state.originalPath = String(params.originalPath); }
    state.job = jb;
    var batch = Math.max(1, options.batch);
    var interval = Math.max(1, Math.round(1000 * batch / Math.max(1, options.cuesPerSecond)));
    function step() {
      jb.timer = null;
      if (state.job !== jb) { return; }
      var cues = [];
      for (var k = 0; k < batch && jb.done < jb.indices.length; k++) {
        var cue = state.cues[jb.indices[jb.done]];
        jb.done++;
        if (!cue) { continue; }
        if (jb.orberg) { orbergCue(cue, jb.orberg); } else { translateCue(cue, jb.random); }
        jb.stats[cue.confidence]++;
        cues.push(view(cue));
      }
      if (cues.length) { emit({ event: 'translate.cue', jobId: id, cues: cues }); }
      var elapsed = Math.max(1, now() - started) / 1000;
      var rate = jb.done / elapsed;
      emit({ event: 'translate.progress', jobId: id, done: jb.done, total: jb.indices.length, cuesPerSec: Math.round(rate * 10) / 10, etaSec: Math.round((jb.indices.length - jb.done) / Math.max(rate, 0.1)) });
      if (jb.done >= jb.indices.length) {
        state.job = null;
        noteChange();
        emit({ event: 'translate.done', jobId: id, stats: { done: jb.done, translated: jb.done, total: jb.indices.length, ok: jb.stats.ok, check: jb.stats.check, fix: jb.stats.fix, cancelled: false } });
        return;
      }
      jb.timer = window.setTimeout(step, interval);
    }
    jb.timer = window.setTimeout(step, interval);
    return { jobId: id };
  }

  function now() {
    return window.performance && typeof window.performance.now === 'function' ? window.performance.now() : new Date().getTime();
  }

  function stripMacrons(s) {
    return s.replace(/[āēīōūȳĀĒĪŌŪȲ]/g, function (c) { return MACRON_PLAIN[c]; });
  }

  var COMMANDS = {
    'engine.hello': function () {
      return {
        version: VERSION, mock: true,
        lexicons: options.noLexicon ? [
          { lang: 'la', path: 'mock://data/lexicons/latin.vpl', available: false, error: { code: 'lexicon_missing', message: 'file not found' } }
        ] : [
          { lang: 'la', path: 'mock://la.vpl', version: 'mock-1', lemmas: 54199, tiers: { t1: 492, t2: 3247, t3: 59340 }, notice: NOTICE_LA },
          { lang: 'grc', path: 'mock://grc.vpl', version: 'mock-1', lemmas: 23169, tiers: { t1: 260, t2: 292, t3: 23025 }, notice: NOTICE_GRC }
        ],
        model: { available: state.model.available, path: state.model.path, sizeBytes: state.model.sizeBytes, cpuOk: true },
        threads: 4, dataDir: 'mock://data'
      };
    },
    'engine.ping': function () { return {}; },
    'engine.shutdown': function () { return {}; },
    'settings.get': function () { return clone(state.settings); },
    'settings.set': function (p) {
      var patch = p.patch || {};
      if (patch.textScale !== undefined && (typeof patch.textScale !== 'number' || patch.textScale < 90 || patch.textScale > 140)) {
        throw fail('bad_params', 'textScale must be 90..140', 'Pick a text size between 90 and 140 percent.');
      }
      merge(state.settings, patch);
      saveSettings();
      return clone(state.settings);
    },
    'project.new': newProject,
    'project.open': openProject,
    'project.save': function (p) {
      needProject();
      var path = p.path || state.project.path;
      if (!path) { throw fail('bad_params', 'no path', 'Choose where to save the project.'); }
      state.project.path = path;
      stopAutosave();
      addRecent(path);
      return { path: path, at: new Date().toISOString() };
    },
    'project.saveAs': function (p) {
      needProject();
      if (!p.path) { throw fail('bad_params', 'no path', 'Choose where to save the project.'); }
      state.project.path = p.path;
      state.project.name = String(p.path).split(/[\\\/]/).pop();
      stopAutosave();
      addRecent(p.path);
      return { path: p.path, at: new Date().toISOString() };
    },
    'project.recover': function (p) {
      var r = openProject({ path: p.path || 'recovered.vpoeta' });
      return { path: r.project.path, at: new Date().toISOString(), project: r.project, warnings: [] };
    },
    'project.close': function () {
      var path = state.project ? state.project.path : null;
      stopJob();
      stopAutosave();
      state.project = null;
      state.cues = [];
      state.undo = [];
      state.redo = [];
      return { path: path, at: new Date().toISOString() };
    },
    'cue.page': function (p) {
      needProject();
      var from = Math.max(0, p.from || 0);
      var count = p.count === undefined ? 50 : p.count;
      if (count > 200) { throw fail('bad_params', 'count above 200', 'Ask for at most 200 cues at once.'); }
      return { total: state.cues.length, cues: state.cues.slice(from, from + count).map(view) };
    },
    'cue.get': function (p) {
      var cue = cueAt(p.index);
      var out = { cue: view(cue), alternatives: alternativesOf(cue), tokens: tokensOf(cue.target), checks: checksOf(cue), reasons: reasonsOf(cue) };
      if (cue.meaning) { out.meaning = clone(cue.meaning); }
      if (state.originalPath && cue.sentence !== undefined) { out.original = SENTENCES[cue.sentence].en; }
      return out;
    },
    'cue.set': function (p) {
      var cue = cueAt(p.index);
      var text = String(p.text || '');
      // Same text again with remember set: only the correction is added (no undo step).
      if (!(p.remember && text === cue.target && cue.state === 'edited')) {
        pushUndo([cue]);
      }
      cue.target = text;
      noteChange();
      cue.lines = breakTwo(cue.target);
      cue.state = 'edited';
      var out = { cue: view(cue) };
      if (p.remember) {
        var key = String(cue.source).toLowerCase();
        var hit = state.corrections.filter(function (c) { return c.key === key; })[0];
        if (hit) {
          hit.target = cue.target;
          hit.scope = p.remember;
        } else {
          hit = { id: 'c' + (state.nextCorrection++), key: key, target: cue.target, scope: p.remember, count: 1 };
          state.corrections.push(hit);
        }
        out.correctionAdded = hit.id;
      }
      return out;
    },
    'cue.choose': function (p) {
      var cue = cueAt(p.index);
      var alts = alternativesOf(cue);
      if (!alts[p.alternative]) { throw fail('bad_params', 'no such alternative', 'Choose 1, 2 or 3.'); }
      pushUndo([cue]);
      cue.target = alts[p.alternative].text;
      noteChange();
      cue.lines = breakTwo(cue.target);
      cue.state = 'edited';
      return { cue: view(cue) };
    },
    'cue.review': function (p) {
      needProject();
      var touched = (p.indices || []).map(function (i) { return state.cues[i]; }).filter(function (c) { return c && c.state !== 'new'; });
      if (touched.length) {
        pushUndo(touched);
        noteChange();
      }
      touched.forEach(function (c) {
        if (p.reviewed === false) {
          if (c.state === 'reviewed') { c.state = c.prevState || 'translated'; }
        } else if (c.state !== 'reviewed') {
          c.prevState = c.state;
          c.state = 'reviewed';
        }
      });
      return { count: touched.length };
    },
    'translate.start': startTranslate,
    'orbergise.start': function (p) { return startTranslate(p, true); },
    'translate.cancel': function (p) {
      var jb = state.job;
      if (jb && (!p.jobId || p.jobId === jb.id)) {
        stopJob();
        emit({ event: 'translate.done', jobId: jb.id, stats: { translated: jb.done, ok: jb.stats.ok, check: jb.stats.check, fix: jb.stats.fix, cancelled: true } });
      }
      return {};
    },
    'word.inspect': function (p) {
      var form = String(p.text || '').toLowerCase();
      var f = FORMS[form];
      if (!f) {
        var seen = knownForms()[plainKey(form)];
        if (seen) {
          return { analyses: [{ lemma: { id: plainKey(seen), head: seen, pos: '', gender: '', cls: '', tier: 0, tierSource: 'mock', freqRank: 0, whitFreq: '', glossEn: '', glossEs: '', emoji: '', principal: seen, flags: [] }, features: { pos: '', 'case': '', number: '', gender: '', person: '', tense: '', mood: '', voice: '', degree: '' }, display: seen }], suggestions: [] };
        }
        var near = Object.keys(FORMS).filter(function (k) { return stripMacrons(k).charAt(0) === stripMacrons(form).charAt(0); }).slice(0, 3);
        return { analyses: [], suggestions: near };
      }
      return { analyses: [{ lemma: lemmaView(f[0]), features: featureView(f), display: p.text }], suggestions: [] };
    },
    'lemma.get': function (p) {
      if (!LEMMAS[p.id]) { throw fail('not_found', 'no lemma ' + p.id, 'That word is not in the dictionary.'); }
      var l = LEMMAS[p.id];
      return { lemma: lemmaView(p.id), senses: [{ glossEn: l.glossEn, glossEs: l.glossEs, keywords: '', tags: [], rank: 0 }], cells: cellsOf(p.id) };
    },
    'names.list': function () {
      needProject();
      return { names: namesList() };
    },
    'names.set': function (p) {
      needProject();
      if (!p.name) { throw fail('bad_params', 'bad name', 'Enter a name.'); }
      if (['keep', 'decline', 'translate'].indexOf(p.policy) < 0) { throw fail('bad_params', 'bad policy', 'Choose keep, decline or translate.'); }
      var e = namesList().filter(function (n) { return n.name === p.name; })[0] || { name: p.name, form: '', gender: '', declension: '' };
      e = { name: p.name, policy: p.policy, form: p.form === undefined ? e.form : String(p.form), gender: p.gender === undefined ? e.gender : String(p.gender), declension: p.declension === undefined ? e.declension : String(p.declension) };
      state.names = state.names.filter(function (n) { return n.name !== p.name; }).concat([e]);
      var affected = [];
      state.cues.forEach(function (c) {
        if (new RegExp('(^|[^A-Za-zÀ-ž])' + p.name + '($|[^A-Za-zÀ-ž])').test(c.source)) {
          affected.push(c.index);
          if (c.state === 'translated') { c.state = 'stale'; }
        }
      });
      noteChange();
      return { affectedCues: affected };
    },
    'corrections.list': function () { return { corrections: clone(state.corrections) }; },
    'corrections.remove': function (p) {
      state.corrections = state.corrections.filter(function (c) { return c.id !== p.id; });
      return { corrections: clone(state.corrections) };
    },
    'words.list': function () {
      needProject();
      return wordsList();
    },
    'export.preview': function (p) {
      needProject();
      return {
        cues: (p.indices || []).map(function (i) {
          var c = cueAt(i);
          var text = c.target || c.source;
          if (p.macrons !== true) { text = stripMacrons(text); }
          if (p.emoji !== true) { text = text.replace(/[\ud83c-\ud83e][\udc00-\udfff]\ufe0f?/g, '').replace(/\s+$/, ''); }
          return { index: i, lines: p.rebreak === false ? [text] : breakTwo(text) };
        })
      };
    },
    'export.write': function (p) {
      needProject();
      if (!p.path) { throw fail('bad_params', 'no path', 'Choose where to save the file.'); }
      if (/exists/i.test(p.path) && !p.overwrite) { throw fail('io', 'file exists: ' + p.path, 'Choose another name or allow replacing the file.'); }
      state.lastExport = clone(p);
      var warnings = [];
      state.cues.forEach(function (c) { if (!c.target) { warnings.push({ index: c.index, kind: 'untranslated' }); } });
      return { path: p.path, warnings: warnings };
    },
    'model.status': modelStatus,
    'model.locate': function (p) {
      var path = String(p.path || '');
      if (!path || /missing/i.test(path)) { throw fail('not_found', 'no file at ' + path, 'The model file was not found.'); }
      if (!/\.gguf$/i.test(path)) { throw fail('model_unsupported_cpu', 'not a model file', 'Choose the .gguf model file.'); }
      state.model = { available: true, path: path, sizeBytes: 391864320, loaded: false, lastLoadMs: 0 };
      state.settings.modelPath = path;
      saveSettings();
      return modelStatus();
    },
    'model.test': function () {
      if (!state.model.available) { throw fail('model_missing', 'no model', 'Install the local model first.'); }
      state.model.loaded = true;
      state.model.lastLoadMs = 1840;
      return modelStatus();
    },
    'model.unload': function () {
      state.model.loaded = false;
      return modelStatus();
    },
    'shell.revealFile': function (p) {
      state.shell.push({ cmd: 'shell.revealFile', path: p.path });
      return {};
    },
    'shell.openExternal': function (p) {
      if (!/^https?:\/\/[^\/?#]/.test(String(p.url || ''))) { throw fail('bad_params', 'only http(s) URLs', 'This link cannot be opened.'); }
      state.shell.push({ cmd: 'shell.openExternal', url: p.url });
      return {};
    },
    'online.test': function () {
      if (!state.settings.engines.online) { throw fail('online_disabled', 'online check is off', 'Turn on the online check in Settings first.'); }
      return { ok: true, latencyMs: 120, message: 'mock' };
    },
    'history.undo': function () {
      var e = state.undo.pop();
      if (!e) { return { canUndo: false, canRedo: state.redo.length > 0, changedIndices: [] }; }
      state.redo.push(restore(e));
      return { canUndo: state.undo.length > 0, canRedo: true, changedIndices: e.map(function (s) { return s.index; }) };
    },
    'history.redo': function () {
      var e = state.redo.pop();
      if (!e) { return { canUndo: state.undo.length > 0, canRedo: false, changedIndices: [] }; }
      state.undo.push(restore(e));
      return { canUndo: true, canRedo: state.redo.length > 0, changedIndices: e.map(function (s) { return s.index; }) };
    },
    'dialog.openFile': function (p) {
      var f = (p && p.filters) || [];
      var path = f.indexOf('gguf') >= 0 ? 'C:\\Users\\Teacher\\Downloads\\latin-helper.gguf' : (p && p.kind === 'original' ? 'C:\\Users\\Teacher\\Videos\\lesson-3.en.srt' : 'C:\\Users\\Student\\Videos\\lesson-3.srt');
      return { path: path, paths: [path], cancelled: false };
    },
    'dialog.saveFile': function (p) { return { path: 'C:\\Users\\Student\\Documents\\' + (p.suggestedName || 'project.vpoeta') }; }
  };

  function deliver(obj) {
    if (listener) { listener(JSON.stringify(obj)); }
  }

  function emit(obj) {
    counts.events++;
    deliver(obj);
  }

  function handle(msg) {
    var f = options.failNext;
    if (f && (!f.cmd || f.cmd === msg.cmd)) {
      options.failNext = null;
      deliver({ id: msg.id, ok: false, error: { code: f.code || 'internal', message: f.message || 'mock failure', hint: f.hint || '' } });
      return;
    }
    var fn = COMMANDS[msg.cmd];
    if (!fn) {
      deliver({ id: msg.id, ok: false, error: { code: 'not_found', message: 'unknown command ' + msg.cmd, hint: '' } });
      return;
    }
    try {
      deliver({ id: msg.id, ok: true, result: fn(msg.params || {}) });
    } catch (e) {
      deliver({ id: msg.id, ok: false, error: e.engineError || { code: 'internal', message: String(e && e.message), hint: '' } });
    }
  }

  function postMessage(text) {
    if (!state) { reset(); }
    var msg;
    try {
      msg = JSON.parse(text);
    } catch (e) {
      return;
    }
    counts.requests++;
    if (options.latencyMs === 'never') { return; }
    window.setTimeout(function () { handle(msg); }, options.latencyMs);
  }

  window.VP_MockEngine = {
    options: options,
    connect: function (fn) {
      if (!state) { reset(); }
      listener = fn;
    },
    disconnect: function () {
      listener = null;
      if (state) { stopJob(); }
    },
    postMessage: postMessage,
    emit: emit,
    generate: function (n, opts) { return generate(n, opts).map(view); },
    reset: reset,
    sentences: function () { return clone(SENTENCES); },
    stats: function () { return { requests: counts.requests, events: counts.events, cues: state ? state.cues.length : 0, job: state && job() ? job().id : null }; },
    shellCalls: function () { return state ? clone(state.shell) : []; },
    lastExport: function () { return state && state.lastExport ? clone(state.lastExport) : null; }
  };
}());
