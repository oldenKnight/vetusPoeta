/* vp_words.js - the "Words in file" tab (DESIGN 13; PREDESIGN 1.2, 4.6): the vocabulary of
 * the translated text from words.list, sorted by count with tier badges, a filter by tier,
 * the tier share bar (% of words per tier, with names), "Copy as list" (one lemma per line)
 * and "Copy as CSV" for flashcards (lemma, gloss, tier, count). The list renders at most
 * ROW_CAP rows (the rest is counted in a hint) so the DOM budget holds for big files.
 * B9 (defensive): entries whose lemma head has no letter (punctuation that leaked into the
 * tokens) are dropped, and brackets or quotes stuck to a head ("((caelum") are trimmed. The
 * "above the level" line reads defaultFidelity on the engine's scale (1 faithful = tier 3
 * allowed .. 3 flexible = tier 1).
 *
 * VP_Words.mount(el) / destroy(); reload() -> Promise; setFilter('all'|'1'|'2'|'3');
 * filter(); listText(words) / csv(words) (pure); copyList() / copyCsv() -> Promise; stats()
 */
(function () {
  'use strict';

  var OWNER = 'words';
  var ROW_CAP = 300;
  var FILTERS = ['all', '1', '2', '3'];
  var s = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }
  function P() { return window.Promise; }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function dstLang() {
    var p = window.VP_Store.get('project') || {};
    var parts = String(p.pair || 'en-la').split('-');
    return parts[1] || parts[0];
  }

  function display(text) {
    var st = window.VP_Store.get('settings') || {};
    return st.showMacrons === false && window.VP_CueList ? window.VP_CueList.stripMacrons(String(text || '')) : String(text || '');
  }

  // Letters of the scripts the lexicons use (Latin with macrons and breves, Greek, polytonic).
  var LETTER_RE = /[A-Za-z\u00AA\u00B5\u00BA\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02AF\u0370-\u03FF\u1E00-\u1FFF]/;
  var EDGE_PUNCT_RE = /^[\s()\[\]{}"\u201C\u201D\u00AB\u00BB.,;:?!\u00BF\u00A1\u00B7\u0387]+|[\s()\[\]{}"\u201C\u201D\u00AB\u00BB.,;:?!\u00BF\u00A1\u00B7\u0387]+$/g;

  function fullHead(w) { return String((w && w.lemma && w.lemma.head) || '').replace(EDGE_PUNCT_RE, ''); }
  function head(w) { return fullHead(w).split(/,\s*/)[0]; }
  function isWord(w) { return LETTER_RE.test(fullHead(w)); }

  function gloss(w) {
    return window.VP_Inspector ? window.VP_Inspector.glossOf(w.lemma).text : ((w.lemma && (w.lemma.glossEn || w.lemma.glossEs)) || '');
  }

  function tierOf(w) { return Number(w.tier || (w.lemma && w.lemma.tier) || 0); }

  function csvCell(v) {
    var x = String(v === undefined || v === null ? '' : v);
    return /[",\r\n]/.test(x) ? '"' + x.replace(/"/g, '""') + '"' : x;
  }

  function listText(words) {
    return (words || []).map(function (w) { return head(w) || fullHead(w); }).join('\n') + '\n';
  }

  function csv(words) {
    var rows = [['lemma', 'gloss', 'tier', 'count']];
    (words || []).forEach(function (w) { rows.push([fullHead(w), gloss(w), tierOf(w) || '', w.count || 0]); });
    return rows.map(function (r) { return r.map(csvCell).join(','); }).join('\r\n') + '\r\n';
  }

  function visible() {
    if (!s) { return []; }
    if (s.filter === 'all') { return s.words; }
    var t = Number(s.filter);
    return s.words.filter(function (w) { return tierOf(w) === t; });
  }

  function renderShare() {
    var D = window.VP_Dom;
    D.clear(s.share);
    var sh = s.tierShare || {};
    var parts = [['t1', 'tier.t1.label'], ['t2', 'tier.t2.label'], ['t3', 'tier.t3.label'], ['names', 'words.share.names.label']];
    var any = parts.some(function (p) { return sh[p[0]] > 0; });
    s.share.hidden = !any;
    if (!any) { return; }
    var bar = el('div', { className: 'vp-share-bar', role: 'img', 'aria-label': parts.map(function (p) { return T(p[1]) + ' ' + Math.round((sh[p[0]] || 0) * 100) + '%'; }).join(', ') });
    parts.forEach(function (p) {
      var seg = el('span', { className: 'vp-share-seg vp-share-' + p[0] });
      seg.style.width = (Math.round((sh[p[0]] || 0) * 1000) / 10) + '%';
      bar.appendChild(seg);
    });
    s.share.appendChild(bar);
    s.share.appendChild(el('ul', { className: 'vp-share-legend' }, parts.map(function (p) {
      return el('li', { className: 'vp-share-key vp-share-' + p[0] }, [i18nEl('span', null, p[1]), el('span', { className: 'vp-mono', text: ' ' + Math.round((sh[p[0]] || 0) * 100) + '%' })]);
    })));
    var st = window.VP_Store.get('settings') || {};
    var f = st.defaultFidelity >= 1 && st.defaultFidelity <= 3 ? st.defaultFidelity : 2;
    var top = 4 - f;
    var outside = (top < 2 ? sh.t2 || 0 : 0) + (top < 3 ? sh.t3 || 0 : 0);
    s.share.appendChild(el('p', { className: 'vp-hint', text: T('words.share.outside.label', { pct: Math.round(outside * 100), tier: top }) }));
  }

  function render() {
    if (!s) { return; }
    var D = window.VP_Dom;
    var lang = dstLang();
    FILTERS.forEach(function (f) { s.filterBtns[f].setAttribute('aria-pressed', f === s.filter ? 'true' : 'false'); });
    D.clear(s.list);
    if (!s.loaded) {
      s.list.appendChild(i18nEl('p', 'vp-hint', 'words.loading.label'));
      return;
    }
    var list = visible();
    s.countEl.textContent = T('words.count', { n: list.length });
    if (!list.length) {
      s.list.appendChild(i18nEl('p', 'vp-hint', s.words.length ? 'words.filter.empty' : 'words.list.empty'));
    }
    var shown = list.slice(0, ROW_CAP);
    shown.forEach(function (w) {
      var badge = window.VP_Inspector ? window.VP_Inspector.tierBadge(tierOf(w), true) : null;
      s.list.appendChild(el('li', { className: 'vp-wordrow' }, [
        el('span', { className: 'vp-wordrow-count vp-mono', text: window.VP_I18n.num(w.count || 0) }),
        el('span', { className: 'vp-wordrow-head vp-text', lang: lang, text: display(head(w)) }),
        badge,
        el('span', { className: 'vp-wordrow-gloss vp-hint', text: gloss(w) })
      ]));
    });
    if (list.length > ROW_CAP) { s.list.appendChild(i18nEl('p', 'vp-hint', 'words.more.label', { n: list.length - ROW_CAP })); }
    renderShare();
  }

  function reload() {
    if (!s) { return P().resolve(null); }
    var gen = s.gen;
    return window.VP_Bridge.call('words.list', {}).then(function (r) {
      if (!s || s.gen !== gen) { return null; }
      s.words = ((r && r.words) || []).filter(isWord).sort(function (a, b) { return (b.count || 0) - (a.count || 0) || (head(a) < head(b) ? -1 : 1); });
      s.tierShare = (r && r.tierShare) || null;
      s.loaded = true;
      render();
      return r;
    }, function (err) {
      if (!s || s.gen !== gen) { return null; }
      s.words = [];
      s.loaded = true;
      render();
      showError(err);
      return null;
    });
  }

  function setFilter(f) {
    if (!s || FILTERS.indexOf(f) < 0) { return false; }
    s.filter = f;
    render();
    return true;
  }

  function copy(text, okKey) {
    var cp = window.VP_Names ? window.VP_Names.copyText : null;
    if (!cp) { return P().resolve(false); }
    return cp(text).then(function (ok) {
      window.VP_Toast.show({ key: ok ? okKey : 'words.copy.fail.label', kind: ok ? 'success' : 'error' });
      return ok;
    });
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-words-action');
    var f = btn.getAttribute('data-words-filter');
    if (f) { setFilter(f); } else if (a === 'list') { copy(listText(visible()), 'words.copy.list.done.label'); } else if (a === 'csv') { copy(csv(visible()), 'words.copy.csv.done.label'); } else if (a === 'reload') { reload(); }
  }

  function mount(root) {
    if (s) { destroy(); }
    s = { gen: (mount.gen = (mount.gen || 0) + 1), words: [], tierShare: null, loaded: false, filter: 'all', filterBtns: {}, removers: [] };
    s.list = el('ol', { className: 'vp-word-list' });
    s.share = el('div', { className: 'vp-share', hidden: true });
    s.countEl = el('span', { className: 'vp-words-count vp-hint' });
    s.root = el('div', { className: 'vp-panel vp-panel-words' }, [
      el('div', { className: 'vp-panel-head' }, [i18nEl('h2', 'vp-panel-title', 'words.panel.title'), s.countEl]),
      s.share,
      el('div', { className: 'vp-segmented vp-words-filter', role: 'group', 'data-i18n-aria': 'words.filter.aria', 'aria-label': T('words.filter.aria') }, FILTERS.map(function (f) {
        s.filterBtns[f] = i18nEl('button', 'vp-seg', f === 'all' ? 'words.filter.all.label' : 'tier.t' + f + '.label', null, { type: 'button', 'aria-pressed': 'false', dataset: { wordsFilter: f } });
        return s.filterBtns[f];
      })),
      s.list,
      el('div', { className: 'vp-row' }, [
        i18nEl('button', 'vp-btn vp-btn-secondary', 'words.copy.list.cta', null, { type: 'button', dataset: { wordsAction: 'list' } }),
        i18nEl('button', 'vp-btn vp-btn-secondary', 'words.copy.csv.cta', null, { type: 'button', dataset: { wordsAction: 'csv' } }),
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'words.reload.cta', null, { type: 'button', dataset: { wordsAction: 'reload' } })
      ]),
      i18nEl('p', 'vp-hint', 'words.csv.hint')
    ]);
    root.appendChild(s.root);
    window.VP_Dom.delegate(s.root, 'button', 'click', onClick, { owner: OWNER });
    s.removers.push(window.VP_I18n.onLanguageChanged(render));
    s.removers.push(window.VP_Store.subscribe('settings', render));
    s.removers.push(window.VP_Store.subscribe('job', function (job) { if (!job && s && s.loaded) { reload(); } }));
    render();
    reload();
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    s.words = null;
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s = null;
  }

  function i18nKeys() {
    return ['words.count', 'words.share.outside.label', 'words.more.label', 'words.copy.list.done.label', 'words.copy.csv.done.label', 'words.copy.fail.label', 'words.filter.empty', 'words.list.empty'];
  }

  window.VP_Words = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    reload: reload,
    setFilter: setFilter,
    filter: function () { return s ? s.filter : null; },
    listText: listText,
    csv: csv,
    copyList: function () { return copy(listText(visible()), 'words.copy.list.done.label'); },
    copyCsv: function () { return copy(csv(visible()), 'words.copy.csv.done.label'); },
    words: function () { return s && s.words ? s.words.slice() : []; },
    stats: function () { return s ? { words: s.words.length, shown: Math.min(ROW_CAP, visible().length), filter: s.filter } : null; },
    i18nKeys: i18nKeys
  };
}());
