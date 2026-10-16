/* vp_cuelist.js - the virtualised cue list (DESIGN 13; PREDESIGN 1.2, 4.1, 6.1, 6.2).
 *
 * Rows are 48 px, fixed, so the window is arithmetic: windowFor(scrollTop, viewHeight, n)
 * gives the visible rows plus 8 overscan rows on each side, never more than 40 rows.
 * The DOM keeps only those rows (a recycled pool); the data lives in VP_Store, filled from
 * cue.page in pages of 200: the visible pages first, then the rest in the background (two
 * requests in flight) so filters, search, counts and "next cue to review" can see every cue
 * (VP_Store caps the records at 50,000).
 * With a filter or a search, the selected cue stays in the list until the selection moves
 * (so accepting or editing a cue never makes it vanish under the cursor).
 * The list is a role="listbox" that keeps the focus itself and points at the selected row
 * with aria-activedescendant, so a re-render never loses the focus (rows are recycled, the
 * listbox is not). Rows carry aria-posinset / aria-setsize; the listbox aria-rowcount.
 *
 * VP_CueList.mount(el, {total, kind, pair}) / destroy()
 * select(index, {focus}) -> bool; selected(); next(); prev(); nextReview(dir) -> index or -1
 * setFilter(id) (FILTERS), filter(), setQuery(text), query(), focusSearch(), firstReview()
 * counts() -> {all, review, fix, edited, emoji, fast, names, green, translated, check}
 *   (also published as VP_Store 'cueCounts'); refresh(); setTotal(n); stats()
 * acceptAllGreen() -> Promise; viewIndices() (test hook)
 * Pure helpers: windowFor(scrollTop, viewHeight, count), fold(text) (lower case, macrons,
 * breves, accents and Greek diacritics removed), needsReview(cue), matches(cue, id)
 */
(function () {
  'use strict';

  var OWNER = 'cuelist';
  var ROW_H = 48;
  var OVERSCAN = 8;
  var MAX_ROWS = 40;
  var PAGE = 200;
  var IN_FLIGHT = 2;
  var DEFAULT_VIEW_H = 640;
  var RECOMPUTE_MS = 250;
  var SEARCH_MS = 150;
  var FILTERS = ['all', 'review', 'fix', 'edited', 'emoji', 'fast', 'names'];
  var EMOJI_RE = /[☀-➿]|[\ud83c-\ud83e][\udc00-\udfff]/;
  var MARKS_RE = /[̀-ͯ]/g;
  var WIDE_RE = /[^\u0000-¿]/;
  // Base letters for Latin-1/Extended-A/B, Greek and Greek Extended (generated with
  // Python's unicodedata: first letter of the NFD form, lower case; final sigma -> sigma).
  var FOLD = [
    [0x00C0, 'aaaaaaæceeeeiiiiðnooooo\u00d7øuuuuyþßaaaaaaæceeeeiiiiðnooooo\u00f7øuuuuyþyaaaaaaccccccccddđđeeeeeeeeeegggggggghhħħiiiiiiiiiıĳĳjjkkĸllllllŀŀłłnnnnnnŉŋŋooooooœœrrrrrrssssssssttttŧŧuuuuuuuuuuuuwwyyyzzzzzzſƀɓƃƃƅƅɔƈƈɖɗƌƌƍǝəɛƒƒɠɣƕɩɨƙƙƚƛɯɲƞɵooƣƣƥƥʀƨƨʃƪƫƭƭʈuuʊʋƴƴƶƶʒƹƹƺƻƽƽƾƿǀǁǂǃǆǆǆǉǉǉǌǌǌaaiioouuuuuuuuuuǝaaaaææǥǥggkkooooʒʒjǳǳǳggƕƿnnaaææøøaaaaeeeeiiiioooorrrruuuussttȝȝhhƞȡȣȣȥȥaaeeooooooooyyȴȵȶȷȸȹⱥȼȼƚⱦȿɀɂɂƀʉʌɇɇɉɉɋɋɍɍɏɏ'],
    [0x0370, 'ͱͱͳͳʹ\u0375ͷͷ\u0378\u0379ͺͻͼͽ\u037eϳ\u0380\u0381\u0382\u0383\u0384\u0385α\u0387εηι\u038bο\u038dυωιαβγδεζηθικλμνξοπρ\u03a2στυφχψωιυαεηιυαβγδεζηθικλμνξοπρσστυφχψωιυουωϗϐϑϒϒϒϕϖϗϙϙϛϛϝϝϟϟϡϡϣϣϥϥϧϧϩϩϫϫϭϭϯϯϰϱϲϳθϵ\u03f6ϸϸϲϻϻϼͻͼͽ'],
    [0x1F00, 'ααααααααααααααααεεεεεε\u1f16\u1f17εεεεεε\u1f1e\u1f1fηηηηηηηηηηηηηηηηιιιιιιιιιιιιιιιιοοοοοο\u1f46\u1f47οοοοοο\u1f4e\u1f4fυυυυυυυυ\u1f58υ\u1f5aυ\u1f5cυ\u1f5eυωωωωωωωωωωωωωωωωααεεηηιιοουυωω\u1f7e\u1f7fααααααααααααααααηηηηηηηηηηηηηηηηωωωωωωωωωωωωωωωωααααα\u1fb5ααααααα\u1fbdι\u1fbf\u1fc0\u1fc1ηηη\u1fc5ηηεεηηη\u1fcd\u1fce\u1fcfιιιι\u1fd4\u1fd5ιιιιιι\u1fdc\u1fdd\u1fde\u1fdfυυυυρρυυυυυυρ\u1fed\u1fee\u1fef\u1ff0\u1ff1ωωω\u1ff5ωωοοωωω\u1ffd\u1ffe\u1fff']
  ];
  var MACRONS = { 'ā': 'a', 'ē': 'e', 'ī': 'i', 'ō': 'o', 'ū': 'u', 'ȳ': 'y', 'Ā': 'A', 'Ē': 'E', 'Ī': 'I', 'Ō': 'O', 'Ū': 'U', 'Ȳ': 'Y', 'ᾱ': 'α', 'ῑ': 'ι', 'ῡ': 'υ', 'Ᾱ': 'Α', 'Ῑ': 'Ι', 'Ῡ': 'Υ' };
  var MACRON_RE = /[āēīōūȳĀĒĪŌŪȲᾱῑῡᾹῙῩ]|̄/g;

  var s = null;

  // ---------------------------------------------------------------- pure helpers
  function windowFor(scrollTop, viewHeight, count) {
    var visible = Math.min(MAX_ROWS, Math.max(1, Math.ceil(Math.max(0, viewHeight) / ROW_H) + 1));
    var first = Math.floor(Math.max(0, scrollTop) / ROW_H);
    first = Math.max(0, Math.min(first, count - Math.min(visible, count)));
    var from = Math.max(0, first - OVERSCAN);
    var to = Math.min(count, first + visible + OVERSCAN);
    if (to - from > MAX_ROWS) {
      var spare = MAX_ROWS - Math.min(visible, MAX_ROWS);
      from = first - Math.min(first - from, Math.floor(spare / 2));
      to = Math.min(count, from + MAX_ROWS);
    }
    return { from: from, to: to, first: first, visible: visible };
  }

  function fold(text) {
    var t = String(text === null || text === undefined ? '' : text).toLowerCase();
    if (WIDE_RE.test(t)) {
      var out = '';
      for (var i = 0; i < t.length; i++) {
        var c = t.charCodeAt(i);
        var ch = t.charAt(i);
        for (var r = 0; r < FOLD.length; r++) {
          if (c >= FOLD[r][0] && c < FOLD[r][0] + FOLD[r][1].length) {
            ch = FOLD[r][1].charAt(c - FOLD[r][0]);
            break;
          }
        }
        out += ch;
      }
      t = out;
    }
    return t.replace(MARKS_RE, '');
  }

  function stripMacrons(text) {
    return String(text || '').replace(MACRON_RE, function (c) { return MACRONS[c] || ''; });
  }

  function isOpen(c) { return c.state === 'translated' || c.state === 'stale'; }

  function needsReview(c) { return !!c && isOpen(c) && (c.confidence === 'check' || c.confidence === 'fix'); }

  function hasFlag(c, f) { return !!(c.flags && c.flags.indexOf(f) >= 0); }

  function cpsLimit() {
    var st = window.VP_Store.get('settings');
    return st && st.cps && typeof st.cps.adult === 'number' ? st.cps.adult : 17;
  }

  function matches(c, id, limit) {
    switch (id) {
    case 'review': return needsReview(c);
    case 'fix': return isOpen(c) && c.confidence === 'fix';
    case 'edited': return c.state === 'edited';
    case 'emoji': return hasFlag(c, 'emoji') || EMOJI_RE.test(c.target || '');
    case 'fast': return hasFlag(c, 'fast') || (c.cps || 0) > (limit === undefined ? cpsLimit() : limit);
    case 'names': return hasFlag(c, 'unknownName');
    default: return true;
    }
  }

  function langs(pair) {
    var p = String(pair || 'en-la').split('-');
    return { src: p[0], dst: p[1] || p[0] };
  }

  // ---------------------------------------------------------------- data loading
  function request(page) {
    if (!s || s.pending[page] || s.loaded[page]) { return; }
    var gen = s.gen;
    s.pending[page] = true;
    s.inflight++;
    window.VP_Bridge.call('cue.page', { from: page * PAGE, count: PAGE }).then(function (r) {
      if (!s || s.gen !== gen) { return; }
      delete s.pending[page];
      s.inflight--;
      s.loaded[page] = true;
      if (typeof r.total === 'number' && r.total !== s.total) { setTotal(r.total); }
      window.VP_Store.putCues(r.cues || []);
      backfill();
    }, function (err) {
      if (!s || s.gen !== gen) { return; }
      delete s.pending[page];
      s.inflight--;
      s.failedPages++;
      if (window.console) { window.console.warn('[VP_CueList] cue.page ' + page + ' failed: ' + (err && err.code)); }
    });
  }

  function pageCount() { return Math.ceil(s.total / PAGE); }

  function backfill() {
    if (!s) { return; }
    var cap = window.VP_Store.cueCap();
    while (s.inflight < IN_FLIGHT && s.next < pageCount() && window.VP_Store.cueCount() < cap) {
      var p = s.next++;
      if (!s.loaded[p] && !s.pending[p]) { request(p); }
    }
  }

  function allLoaded() { return !!s && window.VP_Store.cueCount() >= Math.min(s.total, window.VP_Store.cueCap()); }

  function ensureRange(from, to) {
    for (var p = Math.floor(from / PAGE); p <= Math.floor(Math.max(from, to - 1) / PAGE) && p < pageCount(); p++) {
      if (!s.loaded[p] && !s.pending[p]) { request(p); }
    }
  }

  function setTotal(n) {
    if (!s) { return; }
    s.total = Math.max(0, n | 0);
    window.VP_Store.setCueTotal(s.total);
    markDirty(true);
  }

  // ---------------------------------------------------------------- filtering and counts
  function recompute() {
    var limit = cpsLimit();
    var q = fold(s.query).replace(/^\s+|\s+$/g, '');
    var counts = { all: s.total, review: 0, fix: 0, edited: 0, emoji: 0, fast: 0, names: 0, green: 0, translated: 0, check: 0, loaded: 0 };
    var identity = s.filter === 'all' && !q;
    var view = identity ? null : [];
    s.sticky = null;
    for (var i = 0; i < s.total; i++) {
      var c = window.VP_Store.getCue(i);
      if (!c) { continue; }
      counts.loaded++;
      if (c.state !== 'new') { counts.translated++; }
      if (isOpen(c)) {
        if (c.confidence === 'ok') { counts.green++; } else if (c.confidence === 'check') {
          counts.check++;
          counts.review++;
        } else if (c.confidence === 'fix') {
          counts.fix++;
          counts.review++;
        }
      }
      if (c.state === 'edited') { counts.edited++; }
      if (matches(c, 'emoji')) { counts.emoji++; }
      if (matches(c, 'fast', limit)) { counts.fast++; }
      if (hasFlag(c, 'unknownName')) { counts.names++; }
      if (view) {
        if (matches(c, s.filter, limit) && (!q || fold(c.source).indexOf(q) >= 0 || fold(c.target).indexOf(q) >= 0)) {
          view.push(i);
        } else if (i === s.selected && !s.resetSticky) {
          view.push(i);
          s.sticky = i;
        }
      }
    }
    s.view = view;
    s.resetSticky = false;
    s.counts = counts;
    s.dirty = false;
    s.lastRecompute = now();
    window.VP_Store.set('cueCounts', counts);
    renderHead();
  }

  function now() {
    return window.performance && typeof window.performance.now === 'function' ? window.performance.now() : new Date().getTime();
  }

  function markDirty(urgent) {
    if (!s) { return; }
    s.dirty = true;
    if (urgent) { s.urgent = true; }
    scheduleRender();
  }

  function viewLength() { return s.view ? s.view.length : s.total; }

  function indexAt(pos) { return s.view ? s.view[pos] : pos; }

  // Position of index in the view, or -(insertion point) - 1 when it is not there.
  function posOf(index) {
    if (!s.view) { return index >= 0 && index < s.total ? index : -(Math.max(0, Math.min(index, s.total))) - 1; }
    var lo = 0;
    var hi = s.view.length - 1;
    while (lo <= hi) {
      var mid = (lo + hi) >> 1;
      if (s.view[mid] === index) { return mid; }
      if (s.view[mid] < index) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return -lo - 1;
  }

  // ---------------------------------------------------------------- rendering
  function svgIcon(cls, d) {
    var D = window.VP_Dom;
    return D.el('svg', { className: 'vp-icon ' + cls, viewBox: '0 0 16 16', 'aria-hidden': 'true', focusable: 'false' }, [D.el('path', { d: d })]);
  }

  function makeRow() {
    var D = window.VP_Dom;
    var row = D.el('div', { className: 'vp-cue-row', role: 'option', 'aria-selected': 'false' }, [
      D.el('span', { className: 'vp-cue-num' }),
      D.el('span', { className: 'vp-cue-chip' }, [D.el('span', { className: 'vp-chip-shape', 'aria-hidden': 'true' }), D.el('span', { className: 'vp-cue-chip-word' })]),
      D.el('span', { className: 'vp-cue-text' }),
      svgIcon('vp-cue-lock', 'M5 7.5V5.5a3 3 0 0 1 6 0v2M4 7.5h8v6H4z'),
      D.el('span', { className: 'vp-cue-stale', 'aria-hidden': 'true' })
    ]);
    return row;
  }

  function statusKey(c) {
    if (!c) { return 'cue.row.loading.label'; }
    if (c.state === 'new') { return 'cue.row.new.label'; }
    if (c.state === 'reviewed') { return 'cue.row.reviewed.label'; }
    if (c.state === 'edited') { return 'cue.row.edited.label'; }
    if (c.state === 'stale') { return 'cue.row.stale.label'; }
    return 'confidence.' + c.confidence + '.tooltip';
  }

  function rowText(c, ls) {
    var showMacrons = !(window.VP_Store.get('settings') && window.VP_Store.get('settings').showMacrons === false);
    var raw = c.target ? c.target : c.source;
    var first = String(raw || '').split(/\r?\n/)[0].replace(/<[^>]*>|\{\\[^}]*\}/g, '');
    return { text: showMacrons ? first : stripMacrons(first), lang: c.target ? ls.dst : ls.src, source: !c.target };
  }

  function fillRow(row, index, pos, n) {
    var T = window.VP_I18n;
    var c = window.VP_Store.getCue(index);
    var kids = row.childNodes;
    var num = kids[0];
    var chip = kids[1];
    var text = kids[2];
    var lock = kids[3];
    var stale = kids[4];
    var label = c ? String(c.idRaw || index + 1) : String(index + 1);
    row.id = 'vp-cue-' + index;
    row.setAttribute('data-index', String(index));
    row.setAttribute('aria-posinset', String(pos + 1));
    row.setAttribute('aria-setsize', String(n));
    row.setAttribute('aria-selected', index === s.selected ? 'true' : 'false');
    num.textContent = s.kind === 'text' ? String(index + 1) : label;
    var cls = 'vp-cue-row';
    if (index === s.selected) { cls += ' vp-cue-row-selected'; }
    if (!c) {
      cls += ' vp-cue-row-loading';
      chip.hidden = true;
      text.textContent = T.t('cue.row.loading.label');
      text.className = 'vp-cue-text';
      text.removeAttribute('lang');
      lock.setAttribute('class', 'vp-icon vp-cue-lock');
      lock.setAttribute('hidden', '');
      stale.hidden = true;
      row.className = cls;
      row.setAttribute('aria-label', T.t('cue.row.aria', { n: label, status: T.t('cue.row.loading.label'), text: '' }));
      return;
    }
    var conf = c.state === 'new' ? 'new' : (c.confidence || 'check');
    chip.hidden = c.state === 'new';
    chip.className = 'vp-cue-chip vp-cue-chip-' + conf;
    chip.setAttribute('title', c.state === 'new' ? '' : T.t('confidence.' + conf + '.tooltip'));
    chip.childNodes[1].textContent = c.state === 'new' ? '' : T.t('confidence.' + conf + '.label');
    var rt = rowText(c, s.langs);
    text.textContent = rt.text;
    text.className = 'vp-cue-text vp-text' + (rt.source ? ' vp-cue-text-source' : '');
    text.setAttribute('lang', rt.lang);
    if (c.state === 'reviewed' || c.state === 'edited') { lock.removeAttribute('hidden'); } else { lock.setAttribute('hidden', ''); }
    stale.hidden = c.state !== 'stale';
    if (c.state === 'stale') { stale.setAttribute('title', T.t('cue.row.stale.label')); }
    row.className = cls + ' vp-cue-row-' + c.state;
    row.setAttribute('aria-label', T.t('cue.row.aria', { n: label, status: T.t(statusKey(c)), text: rt.text }));
  }

  function scrollTop() { return Number(s.viewport.scrollTop) || 0; }

  function viewHeight() {
    var v = s.viewport;
    return v.clientHeight || v.offsetHeight || DEFAULT_VIEW_H;
  }

  function scheduleRender() {
    if (!s || s.renderTimer !== null) { return; }
    s.renderTimer = window.VP_Timers.raf(OWNER, function () {
      if (!s) { return; }
      s.renderTimer = null;
      render();
    });
  }

  function render() {
    if (!s) { return; }
    if (s.dirty) {
      var busy = s.inflight > 0 && !s.urgent;
      if (!busy || now() - s.lastRecompute >= RECOMPUTE_MS) {
        s.urgent = false;
        recompute();
      } else if (s.lateTimer === null) {
        s.lateTimer = window.VP_Timers.setTimeout(OWNER, function () {
          if (!s) { return; }
          s.lateTimer = null;
          s.urgent = true;
          render();
        }, RECOMPUTE_MS);
      }
    }
    var n = viewLength();
    var D = window.VP_Dom;
    s.spacer.style.height = (n * ROW_H) + 'px';
    var vh = viewHeight();
    if (scrollTop() > Math.max(0, n * ROW_H - vh)) { s.viewport.scrollTop = Math.max(0, n * ROW_H - vh); }
    var w = windowFor(scrollTop(), vh, n);
    s.win.style.transform = 'translateY(' + (w.from * ROW_H) + 'px)';
    var want = w.to - w.from;
    while (s.rows.length < want) {
      var r = makeRow();
      s.rows.push(r);
      s.win.appendChild(r);
    }
    while (s.rows.length > want) {
      var gone = s.rows.pop();
      gone.parentNode.removeChild(gone);
    }
    var missing = false;
    for (var k = 0; k < want; k++) {
      var idx = indexAt(w.from + k);
      fillRow(s.rows[k], idx, w.from + k, n);
      if (!window.VP_Store.getCue(idx)) { missing = true; }
    }
    s.range = w;
    if (missing && n) { ensureRange(indexAt(w.from), indexAt(w.to - 1) + 1); }
    var activeRow = s.selected !== null && D.qs('#vp-cue-' + s.selected, s.win);
    if (activeRow) { s.viewport.setAttribute('aria-activedescendant', activeRow.id); } else { s.viewport.removeAttribute('aria-activedescendant'); }
    s.viewport.setAttribute('aria-rowcount', String(n));
    s.empty.hidden = n > 0 || s.total === 0 || (!allLoaded() && !s.view);
  }

  function renderHead() {
    if (!s) { return; }
    var T = window.VP_I18n;
    var c = s.counts;
    var opts = s.filterEl.childNodes;
    for (var i = 0; i < opts.length; i++) {
      var id = opts[i].value;
      opts[i].textContent = T.t('cue.filter.' + id + '.label', { n: id === 'all' ? s.total : (c[id] || 0) });
    }
    s.filterEl.value = s.filter;
    s.greenBtn.disabled = !c.green || s.busy;
    s.greenBtn.textContent = T.t('cue.green.cta', { n: c.green });
    s.countEl.textContent = T.t(s.kind === 'text' ? 'cue.list.paragraphs' : 'cue.list.count', { n: s.total });
    var loading = s.total > 0 && c.loaded < Math.min(s.total, window.VP_Store.cueCap());
    s.loadingEl.hidden = !loading;
    if (loading) { s.loadingEl.textContent = T.t('cue.list.loading.label', { done: c.loaded, total: s.total }); }
  }

  function ensureVisible(pos) {
    if (pos < 0) { return; }
    var v = s.viewport;
    var vh = viewHeight();
    var top = pos * ROW_H;
    var cur = scrollTop();
    if (top < cur) { v.scrollTop = top; } else if (top + ROW_H > cur + vh) { v.scrollTop = top + ROW_H - vh; }
  }

  // ---------------------------------------------------------------- selection and moves
  function select(index, opts) {
    if (!s || typeof index !== 'number' || index < 0 || index >= s.total) { return false; }
    var dropSticky = s.sticky !== null && s.sticky !== index;
    s.selected = index;
    if (s.dirty || dropSticky) { recompute(); }
    var pos = posOf(index);
    if (pos >= 0) { ensureVisible(pos); }
    render();
    if (opts && opts.focus) { s.viewport.focus(); }
    var cur = window.VP_Store.get('selection');
    if (!cur || cur.index !== index) { window.VP_Store.set('selection', { index: index }); }
    return true;
  }

  function move(step) {
    if (!s || !viewLength()) { return false; }
    if (s.dirty) { recompute(); }
    var n = viewLength();
    var pos = s.selected === null ? -1 : posOf(s.selected);
    var target;
    if (pos >= 0) { target = pos + step; } else if (s.selected === null) { target = step > 0 ? 0 : n - 1; } else {
      var ins = -pos - 1;
      target = step > 0 ? ins : ins - 1;
    }
    target = Math.max(0, Math.min(n - 1, target));
    return select(indexAt(target));
  }

  function nextReview(dir) {
    if (!s) { return -1; }
    dir = dir < 0 ? -1 : 1;
    var start = s.selected === null ? (dir > 0 ? -1 : s.total) : s.selected;
    for (var i = start + dir; i >= 0 && i < s.total; i += dir) {
      if (needsReview(window.VP_Store.getCue(i))) {
        if (s.view && posOf(i) < 0) { setFilter('review'); }
        select(i);
        return i;
      }
    }
    return -1;
  }

  function firstReview() {
    for (var i = 0; s && i < s.total; i++) { if (needsReview(window.VP_Store.getCue(i))) { return i; } }
    return -1;
  }

  function setFilter(id) {
    if (!s || FILTERS.indexOf(id) < 0) { return false; }
    s.filter = id;
    s.filterEl.value = id;
    s.resetSticky = true;
    recompute();
    var pos = s.selected === null ? -1 : posOf(s.selected);
    if (pos >= 0) { ensureVisible(pos); } else { s.viewport.scrollTop = 0; }
    render();
    return true;
  }

  function setQuery(text) {
    if (!s) { return; }
    s.query = String(text || '');
    if (s.searchEl.value !== s.query) { s.searchEl.value = s.query; }
    s.resetSticky = true;
    recompute();
    var pos = s.selected === null ? -1 : posOf(s.selected);
    if (pos >= 0) { ensureVisible(pos); } else { s.viewport.scrollTop = 0; }
    render();
  }

  function acceptAllGreen() {
    var P = window.Promise;
    if (!s) { return P.resolve(0); }
    var idx = [];
    for (var i = 0; i < s.total; i++) {
      var c = window.VP_Store.getCue(i);
      if (c && isOpen(c) && c.confidence === 'ok') { idx.push(i); }
    }
    if (!idx.length) { return P.resolve(0); }
    s.busy = true;
    renderHead();
    var gen = s.gen;
    var done = function () {
      if (s && s.gen === gen) {
        s.busy = false;
        renderHead();
      }
    };
    return window.VP_Workspace.cmd.review(idx, true, { labelKey: 'workspace.history.acceptGreen.label', toastKey: 'cue.green.done.label' }).then(function (n) {
      done();
      return n;
    }, function (err) {
      done();
      throw err;
    });
  }

  // ---------------------------------------------------------------- events
  function onClick(e, row) {
    var index = Number(row.getAttribute('data-index'));
    select(index, { focus: true });
  }

  function onSearchKey(e) {
    if ((e.key === 'Escape' || e.key === 'Esc') && s.searchEl.value) {
      e.preventDefault();
      e.stopPropagation();
      setQuery('');
    } else if (e.key === 'Enter') {
      e.preventDefault();
      e.stopPropagation();
      if (viewLength()) {
        var pos = s.selected === null ? -1 : posOf(s.selected);
        select(indexAt(pos >= 0 ? pos : 0), { focus: true });
      }
    }
  }

  function build(el) {
    var D = window.VP_Dom;
    s.filterEl = D.el('select', { id: 'vp-cl-filter', className: 'vp-input vp-cl-filter' }, FILTERS.map(function (id) {
      return D.el('option', { value: id });
    }));
    s.searchEl = D.el('input', { id: 'vp-cl-search', type: 'search', className: 'vp-input vp-cl-search', autocomplete: 'off', spellcheck: 'false', 'data-i18n-placeholder': 'cue.search.placeholder', 'data-i18n-title': 'cue.search.tooltip', 'aria-keyshortcuts': 'Control+F' });
    s.greenBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary vp-cl-green' });
    s.countEl = D.el('span', { className: 'vp-cl-count' });
    s.loadingEl = D.el('p', { className: 'vp-cl-loading vp-hint', hidden: true });
    s.win = D.el('div', { className: 'vp-cl-window' });
    s.spacer = D.el('div', { className: 'vp-cl-spacer' }, [s.win]);
    s.viewport = D.el('div', { id: 'vp-cl-list', className: 'vp-cl-viewport', role: 'listbox', tabIndex: 0, 'aria-labelledby': 'vp-cl-title' }, [s.spacer]);
    s.empty = D.el('div', { className: 'vp-cl-empty', hidden: true }, [
      D.el('p', { 'data-i18n': 'cue.list.empty' }),
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-i18n': 'cue.list.showAll.cta', dataset: { clAction: 'showAll' } })
    ]);
    s.root = D.el('div', { className: 'vp-cuelist' }, [
      D.el('div', { className: 'vp-cl-head' }, [
        D.el('div', { className: 'vp-cl-titlerow' }, [
          D.el('h2', { id: 'vp-cl-title', className: 'vp-cl-title', 'data-i18n': s.kind === 'text' ? 'cue.list.paragraphs.title' : 'cue.list.title' }),
          s.countEl
        ]),
        D.el('div', { className: 'vp-cl-tools' }, [
          D.el('label', { htmlFor: 'vp-cl-filter', className: 'vp-visually-hidden', 'data-i18n': 'cue.filter.aria' }),
          s.filterEl,
          D.el('label', { htmlFor: 'vp-cl-search', className: 'vp-visually-hidden', 'data-i18n': 'cue.search.aria' }),
          s.searchEl
        ]),
        s.greenBtn,
        s.loadingEl
      ]),
      s.viewport,
      s.empty
    ]);
    el.appendChild(s.root);
    window.VP_I18n.bind(s.root);
  }

  function mount(el, opts) {
    if (s) { destroy(); }
    opts = opts || {};
    s = {
      gen: (mount.gen = (mount.gen || 0) + 1), total: 0, kind: opts.kind || 'subs', langs: langs(opts.pair),
      selected: null, filter: 'all', query: '', view: null, sticky: null, resetSticky: false, counts: {}, dirty: true, urgent: true, lastRecompute: -1e9,
      rows: [], range: null, pending: {}, loaded: {}, inflight: 0, next: 0, failedPages: 0,
      renderTimer: null, lateTimer: null, searchTimer: null, busy: false, removers: []
    };
    build(el);
    var D = window.VP_Dom;
    D.delegate(s.win, '.vp-cue-row', 'click', onClick, { owner: OWNER });
    D.on(s.viewport, 'scroll', scheduleRender, { owner: OWNER });
    D.on(window, 'resize', scheduleRender, { owner: OWNER });
    D.on(s.filterEl, 'change', function () { setFilter(s.filterEl.value); }, { owner: OWNER });
    D.on(s.searchEl, 'input', function () {
      if (s.searchTimer !== null) { window.VP_Timers.clear(s.searchTimer); }
      s.searchTimer = window.VP_Timers.setTimeout(OWNER, function () {
        s.searchTimer = null;
        setQuery(s.searchEl.value);
      }, SEARCH_MS);
    }, { owner: OWNER });
    D.on(s.searchEl, 'keydown', onSearchKey, { owner: OWNER });
    D.on(s.greenBtn, 'click', function () { acceptAllGreen().then(null, function (err) { window.VP_App.showError(err); }); }, { owner: OWNER });
    D.delegate(s.empty, '[data-cl-action]', 'click', function () {
      setQuery('');
      setFilter('all');
    }, { owner: OWNER });
    s.removers.push(window.VP_Store.subscribe('cues', function () { markDirty(false); }));
    s.removers.push(window.VP_Store.subscribe('settings', function () { markDirty(true); }));
    s.removers.push(window.VP_I18n.onLanguageChanged(function () {
      renderHead();
      render();
    }));
    s.removers.push(window.VP_Debug.registerCache('cueListRows', function () { return s ? s.rows.length : 0; }));
    setTotal(typeof opts.total === 'number' ? opts.total : window.VP_Store.cueTotal());
    recompute();
    render();
    backfill();
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s.rows = null;
    s.view = null;
    s = null;
  }

  function i18nKeys() {
    var keys = ['cue.row.aria', 'cue.list.count', 'cue.list.paragraphs', 'cue.green.cta', 'cue.list.loading.label'];
    for (var i = 0; i < FILTERS.length; i++) { keys.push('cue.filter.' + FILTERS[i] + '.label'); }
    return keys;
  }

  window.VP_CueList = {
    FILTERS: FILTERS.slice(),
    ROW_H: ROW_H,
    MAX_ROWS: MAX_ROWS,
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    select: select,
    selected: function () { return s ? s.selected : null; },
    next: function () { return move(1); },
    prev: function () { return move(-1); },
    nextReview: nextReview,
    firstReview: firstReview,
    setFilter: setFilter,
    filter: function () { return s ? s.filter : null; },
    setQuery: setQuery,
    query: function () { return s ? s.query : ''; },
    focusSearch: function () {
      if (!s) { return false; }
      s.searchEl.focus();
      if (typeof s.searchEl.select === 'function') { s.searchEl.select(); }
      return true;
    },
    focusList: function () { if (s) { s.viewport.focus(); } },
    counts: function () {
      if (s && s.dirty) { recompute(); }
      return s ? s.counts : null;
    },
    refresh: function () { markDirty(true); },
    setTotal: setTotal,
    total: function () { return s ? s.total : 0; },
    allLoaded: allLoaded,
    acceptAllGreen: acceptAllGreen,
    viewIndices: function () {
      if (!s) { return []; }
      if (s.dirty) { recompute(); }
      if (s.view) { return s.view.slice(); }
      var out = [];
      for (var i = 0; i < s.total; i++) { out.push(i); }
      return out;
    },
    renderNow: function () { if (s) { render(); } },
    stats: function () {
      return s ? { rendered: s.rows.length, from: s.range ? s.range.from : 0, to: s.range ? s.range.to : 0, total: s.total, view: viewLength(), loaded: window.VP_Store.cueCount(), inflight: s.inflight, failedPages: s.failedPages } : null;
    },
    windowFor: windowFor,
    fold: fold,
    stripMacrons: stripMacrons,
    needsReview: needsReview,
    isOpen: isOpen,
    matches: matches,
    i18nKeys: i18nKeys
  };
}());
