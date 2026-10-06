/* vp_panes.js - the centre column (DESIGN 13; PREDESIGN 1.2, 4.2, 4.6, 4.7): source pane,
 * target pane with word chips, the textarea editor, the preview strip and alternatives.
 *
 * Source: the cue text with its markup rendered (italic tags as italic, ASS override blocks
 * as small grey chips that are visible but not editable); header with the timing (mono),
 * duration and characters per second (warn colour and the word "fast" over the limit).
 * Target: every word is a focusable span; clicking it (or Enter/Space on it) publishes
 * VP_Store 'inspect' {index, token, text, lang, lemmaId} for the Word inspector and opens a
 * small inline card from word.inspect (LRU 200). Editing (E, the Edit button, or a click
 * between words) swaps in a textarea styled like the pane, with a mirror behind it that
 * underlines unknown words (word.inspect, debounced 300 ms); Esc cancels, Ctrl+Enter keeps
 * the change through cue.set, then the inline chip "Remember this change? [This phrase]
 * [Just this cue]" appears. Token offsets from the engine are UTF-8 bytes, so words are
 * placed by searching their text in order, not by offset.
 * Preview: two lines as a player shows them, with the Export settings for macrons and
 * emoji, a 42-character guide and warning chips for long lines and fast cues.
 * Alternatives: up to 3 (keys 1-3) with their reason. Macron and emoji toggles (settings
 * showMacrons / showEmoji) change only the rendering.
 *
 * B8, reading pairs (la-en, la-es, grc-en, grc-es; the engine flags their cues
 * "source-tokens"): cue.get tokens describe the SOURCE words, so the source pane shows them as
 * word chips (hover or focus -> a small card under the text: lemma head, form in words, gloss
 * in the UI language, tier, emoji, from the token, its "analysis" reason and word.inspect;
 * click / Enter -> VP_Store 'inspect' {side:"analysis"} for the Word tab) and the target pane
 * shows the readable English/Spanish text plainly (no chips). Teachers can turn on the
 * interlinear lines under each source word (lemma | form | gloss; Ctrl+I or the button in the
 * pane head; settings key `interlinear`, off by default). Greek text keeps its ";" question
 * mark and carries lang="grc" wherever it is shown. Alternative reasons are looked up as
 * camel-case keys ("word by word" -> target.alt.reason.wordByWord.label), else shown as sent.
 *
 * VP_Panes.mount(el, {kind, pair}) / destroy(); mode() -> 'empty'|'view'|'edit'|'saving'|
 * 'remember'; startEdit() -> bool; cancelEdit() -> bool; acceptEdit() -> Promise(bool);
 * remember('phrase'|'cue'); chooseAlt(1..3) -> bool; openWord(k); dismiss() -> bool;
 * index(); tokens(); alternatives(); setEditorText(text) (tests); stats();
 * reading() -> bool; sourceTokens(); openSourceWord(k); showCard(k) / hideCard();
 * toggleInterlinear(on?) -> bool | null (null: not a reading pair); interlinear();
 * reasonKey(reason)
 */
(function () {
  'use strict';

  var OWNER = 'panes';
  var DETAIL_CAP = 41;
  var WORD_CAP = 200;
  var CHECK_MS = 300;
  var LINE_MAX = 42;
  var WORD_RE = /[^\s.,;:?!¿¡"“”«»()\[\]{}\-–—·;]+/g;
  var EMOJI_RE = /[☀-➿]️?|[\ud83c-\ud83e][\udc00-\udfff]️?/g;
  var MARKUP_RE = /(<\/?[a-zA-Z][^>]*>|\{\\[^}]*\})/g;
  var CHECK_LANGS = { la: true, grc: true };
  var CLASSICAL = { la: true, grc: true };
  var MODERN = { en: true, es: true };

  var p = null;

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

  function cpsLimit() {
    var st = settings();
    return st.cps && typeof st.cps.adult === 'number' ? st.cps.adult : 17;
  }

  function display(text) {
    return settings().showMacrons === false ? window.VP_CueList.stripMacrons(text) : String(text || '');
  }

  function camel(v) { return String(v).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }

  function term(feature, value) {
    if (!value) { return ''; }
    var key = 'grammar.' + feature + '.' + camel(value) + '.label';
    return window.VP_I18n.has(key) ? window.VP_I18n.t(key) : String(value);
  }

  function langName(code) {
    var key = 'app.lexicon.lang.' + code;
    return window.VP_I18n.has(key) ? window.VP_I18n.t(key) : code;
  }

  function setText(node, key, vars) {
    node.setAttribute('data-i18n', key);
    if (vars) { node.setAttribute('data-i18n-vars', JSON.stringify(vars)); } else { node.removeAttribute('data-i18n-vars'); }
    window.VP_I18n.bind(node);
  }

  function visibleLength(line) { return String(line).replace(MARKUP_RE, '').length; }

  function balance(text) {
    if (text.length <= LINE_MAX) { return [text]; }
    var mid = Math.floor(text.length / 2);
    var best = -1;
    for (var i = 0; i < text.length; i++) {
      if (text.charAt(i) === ' ' && (best < 0 || Math.abs(i - mid) < Math.abs(best - mid))) { best = i; }
    }
    return best < 0 ? [text] : [text.slice(0, best), text.slice(best + 1)];
  }

  function sameCue(a, b) {
    if (!a || !b) { return a === b; }
    return a.target === b.target && a.state === b.state && a.confidence === b.confidence && a.source === b.source &&
      a.cps === b.cps && (a.lines || []).join('\n') === (b.lines || []).join('\n') && (a.flags || []).join() === (b.flags || []).join();
  }

  function cue() { return p && p.index !== null ? window.VP_Store.getCue(p.index) : null; }

  // "word by word" -> target.alt.reason.wordByWord.label
  function reasonKey(reason) {
    var c = String(reason || '').replace(/[^A-Za-z0-9]+([A-Za-z0-9])/g, function (m, ch) { return ch.toUpperCase(); }).replace(/[^A-Za-z0-9]/g, '');
    return 'target.alt.reason.' + c + '.label';
  }

  // The engine's role of a source word in the UI language ("place (to)" -> placeTo), the
  // engine's words when there is no key, nothing for "other".
  function roleText(role) {
    if (!role || role === 'other') { return ''; }
    var k = 'source.word.role.' + String(role).replace(/[^A-Za-z0-9]+([A-Za-z0-9])/g, function (m, ch) { return ch.toUpperCase(); }).replace(/[^A-Za-z0-9]/g, '') + '.label';
    return window.VP_I18n.has(k) ? window.VP_I18n.t(k) : String(role);
  }

  function isReading(c) {
    if (!p) { return false; }
    if (c && (c.flags || []).indexOf('source-tokens') >= 0) { return true; }
    return p.reading;
  }

  function interlinearOn() { return settings().interlinear === true; }

  // ---------------------------------------------------------------- tokens
  function localTokens(text) {
    var out = [];
    var re = new RegExp(WORD_RE.source, 'g');
    var m;
    while ((m = re.exec(text)) !== null) { out.push({ text: m[0], display: m[0], at: m.index }); }
    return out;
  }

  // Engine tokens placed by text search (their offsets are UTF-8 bytes).
  function placeTokens(text, tokens) {
    var out = [];
    var pos = 0;
    for (var i = 0; i < tokens.length; i++) {
      var t = tokens[i];
      var at = text.indexOf(t.text, pos);
      if (at < 0) { return localTokens(text); }
      var o = {};
      for (var k in t) { if (Object.prototype.hasOwnProperty.call(t, k)) { o[k] = t[k]; } }
      o.at = at;
      out.push(o);
      pos = at + t.text.length;
    }
    return out;
  }

  function tokensFor(c) {
    var d = p.detail;
    if (d && d.cue && d.cue.target === c.target && d.tokens && d.tokens.length) { return placeTokens(c.target, d.tokens); }
    return localTokens(c.target || '');
  }

  // ---------------------------------------------------------------- source words (reading pairs)
  function analysisOf(k) {
    var rs = (p.detail && p.detail.reasons) || [];
    for (var i = 0; i < rs.length; i++) { if (rs[i] && rs[i].kind === 'analysis' && rs[i].tokenIndex === k) { return rs[i].data || {}; } }
    return null;
  }

  function srcWordKey(word) { return p.langs.src + '|' + String(word).toLowerCase(); }

  // The lexicon entry of a source word (word.inspect, LRU shared with the target words),
  // matched to the lemma the engine chose.
  function srcLemma(k) {
    var t = p.srcToks[k];
    if (!t) { return null; }
    var r = p.words.get(srcWordKey(t.text));
    var list = (r && r.analyses) || [];
    for (var i = 0; i < list.length; i++) { if (list[i].lemma && t.lemmaId !== undefined && String(list[i].lemma.id) === String(t.lemmaId)) { return list[i].lemma; } }
    return list[0] ? list[0].lemma : null;
  }

  function fetchSrcWord(k) {
    var t = p.srcToks[k];
    if (!t || p.words.get(srcWordKey(t.text)) !== undefined || p.asked[srcWordKey(t.text)]) { return; }
    var gen = p.gen;
    var key = srcWordKey(t.text);
    p.asked[key] = true;
    window.VP_Bridge.call('word.inspect', { text: t.text, lang: p.langs.src }).then(function (r) {
      if (!p || p.gen !== gen) { return; }
      delete p.asked[key];
      p.words.put(key, r);
      if (p.cardTok !== null) { renderCard(p.cardTok); }
      if (interlinearOn()) { renderIlLine(); }
    }, function () {
      if (p && p.gen === gen) { delete p.asked[key]; }
    });
  }

  // Gloss in the UI language: the lexicon entry when known (glossEn / glossEs, "via English"
  // handled by the inspector's helper), else what the engine wrote in the analysis.
  function srcGloss(k, short) {
    var l = srcLemma(k);
    var g = l && window.VP_Inspector ? window.VP_Inspector.glossOf(l).text : '';
    if (!g) {
      var a = analysisOf(k);
      g = a ? (a.gloss || '') : '';
    }
    return short ? String(g).split(/[,;]\s*/)[0] : g;
  }

  function srcInfo(k) {
    var t = p.srcToks[k] || {};
    var a = analysisOf(k) || {};
    var l = srcLemma(k) || {};
    var fw = window.VP_Inspector ? window.VP_Inspector.formWords(t.features || {}) : { words: '', abbr: '' };
    return {
      text: t.text || '', head: a.head || String(l.head || '').split(/,\s*/)[0] || t.text || '', words: fw.words, abbr: fw.abbr,
      gloss: srcGloss(k, false), glossShort: srcGloss(k, true), tier: t.tier || l.tier || 0, emoji: t.emoji || l.emoji || '', unknown: !!t.unknown || (!t.lemmaId && t.lemmaId !== 0)
    };
  }

  function srcChip(k, t, extraCls) {
    var D = window.VP_Dom;
    var attrs = { className: 'vp-word vp-src-word' + (extraCls ? ' ' + extraCls : '') + (t.unknown ? ' vp-word-unknown' : ''), tabIndex: 0, role: 'button', 'data-src-tok': String(k), 'aria-describedby': 'vp-src-card' };
    if (!interlinearOn()) {
      attrs.text = display(t.display || t.text);
      return D.el('span', attrs);
    }
    var info = srcInfo(k);
    return D.el('span', attrs, [
      D.el('span', { className: 'vp-il-word', text: display(t.display || t.text) }),
      D.el('span', { className: 'vp-il-lemma', text: info.unknown ? '–' : display(info.head) }),
      D.el('span', { className: 'vp-il-form', title: info.words || null, text: info.abbr || '–' }),
      D.el('span', { className: 'vp-il-gloss', lang: window.VP_I18n.lang().slice(0, 2), text: info.glossShort || '–' })
    ]);
  }

  // Text of one markup run: plain, or the next source tokens as chips (placed by search, the
  // engine's offsets being UTF-8 bytes of the cue source).
  function appendRun(container, line, cls, chips) {
    var D = window.VP_Dom;
    var target = cls ? D.el('span', { className: cls }) : container;
    if (cls) { container.appendChild(target); }
    if (!chips) {
      target.appendChild(document.createTextNode(line));
      return;
    }
    var pos = 0;
    while (chips.k < chips.list.length) {
      var t = chips.list[chips.k];
      var at = line.indexOf(t.text, pos);
      if (at < 0) { break; }
      if (at > pos) { target.appendChild(document.createTextNode(line.slice(pos, at))); }
      target.appendChild(srcChip(chips.k, t, null));
      chips.k++;
      pos = at + t.text.length;
    }
    if (pos < line.length) { target.appendChild(document.createTextNode(line.slice(pos))); }
  }

  // ---------------------------------------------------------------- source pane
  function renderMarkup(container, text, tokens) {
    var D = window.VP_Dom;
    D.clear(container);
    var chips = tokens && tokens.length ? { list: tokens, k: 0 } : null;
    var italic = 0;
    var bold = 0;
    var parts = String(text || '').replace(/\\[Nn]/g, '\n').split(MARKUP_RE);
    for (var i = 0; i < parts.length; i++) {
      var part = parts[i];
      if (!part) { continue; }
      if (/^<\/?[ie]m?>$/i.test(part) || /^<\/?i\s*>$/i.test(part)) {
        italic += part.charAt(1) === '/' ? -1 : 1;
        continue;
      }
      if (/^<\/?(b|strong)>$/i.test(part)) {
        bold += part.charAt(1) === '/' ? -1 : 1;
        continue;
      }
      if (/^\{\\/.test(part) || /^</.test(part)) {
        var i1 = /\\i1/.exec(part);
        var i0 = /\\i0/.exec(part);
        if (i1) { italic++; }
        if (i0 && italic > 0) { italic--; }
        container.appendChild(D.el('span', { className: 'vp-markup-chip', 'data-i18n-title': 'source.markup.tooltip', title: window.VP_I18n.t('source.markup.tooltip'), text: part }));
        continue;
      }
      var lines = part.split('\n');
      for (var l = 0; l < lines.length; l++) {
        if (l > 0) { container.appendChild(D.el('br')); }
        if (!lines[l]) { continue; }
        appendRun(container, lines[l], italic > 0 || bold > 0 ? (italic > 0 ? 'vp-i' : '') + (bold > 0 ? ' vp-b' : '') : '', chips);
      }
    }
    return chips ? chips.k : 0;
  }

  // Source tokens of a reading pair when cue.get has them for the cue as shown.
  function sourceTokensOf(c) {
    var d = p.detail;
    if (!isReading(c) || !d || !d.cue || d.cue.source !== c.source || !d.tokens || !d.tokens.length) { return []; }
    return d.tokens;
  }

  function renderSource(c) {
    var T = window.VP_I18n;
    var focused = document.activeElement;
    var focusTok = focused && p.srcText.contains(focused) ? focused.getAttribute('data-src-tok') : null;
    p.srcToks = sourceTokensOf(c);
    var placed = renderMarkup(p.srcText, c.source, p.srcToks);
    if (placed < p.srcToks.length) { p.srcToks = p.srcToks.slice(0, placed); }
    p.srcText.className = 'vp-pane-text vp-text' + (p.srcToks.length && interlinearOn() ? ' vp-il' : '');
    var reading = isReading(c);
    p.ilBtn.hidden = !reading;
    p.ilBtn.setAttribute('aria-pressed', interlinearOn() ? 'true' : 'false');
    p.ilBtn.disabled = !p.srcToks.length;
    if (p.cardTok !== null && !p.srcToks[p.cardTok]) { hideCard(); }
    if (p.srcToks.length && interlinearOn()) { for (var k = 0; k < p.srcToks.length; k++) { fetchSrcWord(k); } }
    if (focusTok !== null && focusTok !== undefined) {
      var again = window.VP_Dom.qs('[data-src-tok="' + focusTok + '"]', p.srcText);
      if (again) { again.focus(); }
    }
    if (p.kind === 'text') { return; }
    p.timing.textContent = String(c.timingRaw || '').replace('-->', '→');
    var secs = Math.round((c.durationMs || 0) / 100) / 10;
    p.dur.textContent = T.t('unit.secondsShort', { n: secs });
    var cps = c.cps || (secs ? Math.round(visibleLength(c.source || '') / secs * 10) / 10 : 0);
    var limit = cpsLimit();
    var fast = cps > limit;
    p.cps.className = 'vp-cps' + (fast ? ' vp-cps-fast' : '');
    p.cps.textContent = T.t(fast ? 'source.cps.fast.label' : 'source.cps.label', { n: cps });
    p.cps.setAttribute('title', T.t('source.cps.tooltip', { n: cps, s: secs, limit: limit }));
  }

  // ---------------------------------------------------------------- target pane
  function renderTarget(c) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var focused = document.activeElement;
    var focusTok = focused && p.view.contains(focused) && focused.getAttribute('data-tok');
    var conf = c.state === 'new' ? null : c.confidence;
    p.tgtChip.hidden = !conf;
    if (conf) {
      p.tgtChip.className = 'vp-chip vp-chip-' + conf;
      p.tgtChip.setAttribute('title', T.t('confidence.' + conf + '.tooltip'));
      setText(p.tgtChip.childNodes[1], 'confidence.' + conf + '.label');
    }
    var stateKey = { reviewed: 'target.state.reviewed.label', edited: 'target.state.edited.label', stale: 'target.state.stale.label', 'new': 'target.state.new.label' }[c.state];
    p.tgtState.hidden = !stateKey;
    if (stateKey) { setText(p.tgtState, stateKey); }
    p.acceptBtn.disabled = c.state === 'new' || c.state === 'reviewed';
    D.clear(p.view);
    p.tokens = [];
    if (!c.target) {
      p.view.appendChild(D.el('span', { className: 'vp-target-empty', 'data-i18n': 'target.empty.label', text: T.t('target.empty.label') }));
      return;
    }
    if (isReading(c)) {
      // The readable English/Spanish sentence: plain text, the words are explained in the source pane.
      p.view.appendChild(document.createTextNode(display(c.target)));
      return;
    }
    var text = c.target;
    var toks = tokensFor(c);
    var showEmoji = settings().showEmoji !== false;
    var pos = 0;
    for (var k = 0; k < toks.length; k++) {
      var t = toks[k];
      if (t.at > pos) { p.view.appendChild(document.createTextNode(display(text.slice(pos, t.at)))); }
      var cls = 'vp-word' + (t.unknown ? ' vp-word-unknown' : '');
      var attrs = { className: cls, tabIndex: 0, role: 'button', 'data-tok': String(k), text: display(t.display || t.text) };
      if (t.unknown) { attrs.title = T.t('target.word.unknown.tooltip'); }
      p.view.appendChild(D.el('span', attrs));
      if (t.emoji && showEmoji) { p.view.appendChild(D.el('span', { className: 'vp-emoji vp-word-emoji', role: 'img', 'aria-label': t.text, title: t.text, text: t.emoji })); }
      pos = t.at + t.text.length;
      p.tokens.push(t);
    }
    if (pos < text.length) { p.view.appendChild(document.createTextNode(display(text.slice(pos)))); }
    if (focusTok !== null && focusTok !== false && focusTok !== undefined) {
      var again = D.qs('[data-tok="' + focusTok + '"]', p.view);
      if (again) { again.focus(); } else { p.view.focus(); }
    }
  }

  // ---------------------------------------------------------------- preview
  function renderPreview(c) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    if (p.kind === 'text') { return; }
    var ex = settings()['export'] || {};
    D.clear(p.previewLines);
    D.clear(p.previewWarn);
    if (!c.target) {
      p.previewLines.appendChild(D.el('div', { className: 'vp-preview-none', 'data-i18n': 'target.preview.none.label', text: T.t('target.preview.none.label') }));
      return;
    }
    var lines = c.lines && c.lines.length ? c.lines.slice() : [c.target];
    lines = lines.map(function (l) {
      var x = ex.macrons === true ? String(l) : window.VP_CueList.stripMacrons(l);
      if (ex.emoji !== true) { x = x.replace(EMOJI_RE, '').replace(/\s{2,}/g, ' ').replace(/^\s+|\s+$/g, ''); }
      return x.replace(MARKUP_RE, '');
    });
    var tooLong = lines.length > 2 || lines.some(function (l) { return l.length > LINE_MAX; });
    if (tooLong && ex.rebreak !== false) {
      lines = balance(lines.join(' '));
      tooLong = lines.length > 2 || lines.some(function (l) { return l.length > LINE_MAX; });
    }
    var overflow = tooLong || (c.flags || []).indexOf('overflow') >= 0;
    for (var i = 0; i < lines.length; i++) {
      p.previewLines.appendChild(D.el('div', { className: 'vp-preview-line' + (lines[i].length > LINE_MAX ? ' vp-preview-over' : ''), lang: p.langs.dst, text: lines[i] }));
    }
    var cps = c.cps || 0;
    if (overflow) { p.previewWarn.appendChild(D.el('span', { className: 'vp-warn-chip', 'data-i18n': 'target.preview.overflow.label', 'data-i18n-vars': { n: LINE_MAX }, text: T.t('target.preview.overflow.label', { n: LINE_MAX }) })); }
    if (cps > cpsLimit() || (c.flags || []).indexOf('cps') >= 0) {
      p.previewWarn.appendChild(D.el('span', { className: 'vp-warn-chip', 'data-i18n': 'target.preview.fast.label', 'data-i18n-vars': { n: cps, limit: cpsLimit() }, text: T.t('target.preview.fast.label', { n: cps, limit: cpsLimit() }) }));
    }
  }

  // ---------------------------------------------------------------- alternatives
  function renderAlts(c) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    D.clear(p.altList);
    p.alts = [];
    var d = p.detail;
    if (d && d.alternatives && c) {
      for (var i = 0; i < d.alternatives.length && p.alts.length < 3; i++) {
        var a = d.alternatives[i];
        if (!a || !a.text || a.text === c.target) { continue; }
        p.alts.push({ text: a.text, reason: a.reason || '', engineIndex: i });
      }
    }
    p.altEmpty.hidden = p.alts.length > 0;
    setText(p.altEmpty, d ? 'target.alt.none.label' : 'target.alt.loading.label');
    for (var n = 0; n < p.alts.length; n++) {
      var alt = p.alts[n];
      var rk = reasonKey(alt.reason);
      p.altList.appendChild(D.el('li', null, [D.el('button', { type: 'button', className: 'vp-alt', 'data-alt': String(n + 1), 'aria-keyshortcuts': String(n + 1) }, [
        D.el('kbd', { className: 'vp-alt-num', text: String(n + 1) }),
        D.el('span', { className: 'vp-alt-text vp-text', lang: p.langs.dst, text: display(alt.text) }),
        alt.reason ? D.el('span', { className: 'vp-alt-reason', text: T.has(rk) ? T.t(rk) : alt.reason }) : null
      ])]));
    }
  }

  // ---------------------------------------------------------------- whole render
  function render() {
    if (!p) { return; }
    var c = cue();
    var has = p.index !== null;
    p.emptyEl.hidden = has;
    p.body.hidden = !has;
    if (!has) {
      p.mode = 'empty';
      return;
    }
    if (p.mode === 'empty') { p.mode = 'view'; }
    if (!c) {
      window.VP_Dom.clear(p.srcText);
      window.VP_Dom.clear(p.view);
      p.srcText.appendChild(document.createTextNode(window.VP_I18n.t('cue.row.loading.label')));
      return;
    }
    p.shown = c;
    renderSource(c);
    if (p.mode !== 'edit' && p.mode !== 'saving') { renderTarget(c); }
    renderPreview(c);
    renderAlts(c);
  }

  function fetchDetail(index) {
    var gen = p.gen;
    window.VP_Bridge.call('cue.get', { index: index }).then(function (r) {
      if (!p || p.gen !== gen) { return; }
      p.details.put(index, r);
      if (p.index === index) {
        p.detail = r;
        var c = cue();
        if (c && isReading(c)) { renderSource(c); }
        if (c && p.mode !== 'edit' && p.mode !== 'saving') { renderTarget(c); }
        if (c) { renderAlts(c); }
      }
    }, function (err) {
      if (!p || p.gen !== gen || p.index !== index) { return; }
      p.detail = { alternatives: [], tokens: [], failed: err && err.code };
      renderAlts(cue());
    });
  }

  function show(index) {
    if (!p) { return; }
    if (p.mode === 'edit') { leaveEditor(); }
    if (p.mode === 'remember') { hideRemember(); }
    closePop();
    hideCard();
    p.index = typeof index === 'number' ? index : null;
    p.detail = p.index === null ? null : (p.details.get(p.index) || null);
    if (p.index === null) { p.mode = 'empty'; } else if (p.mode !== 'saving') { p.mode = 'view'; }
    render();
    if (p.index !== null && !p.detail) { fetchDetail(p.index); }
  }

  function onCuesChanged() {
    if (!p || p.index === null) { return; }
    var c = cue();
    if (sameCue(c, p.shown)) { return; }
    if (p.shown && c && (c.target !== p.shown.target || c.state !== p.shown.state)) {
      p.details.drop(p.index);
      p.detail = null;
      fetchDetail(p.index);
    }
    render();
  }

  // ---------------------------------------------------------------- editor
  function setMode(m) {
    p.mode = m;
    var editing = m === 'edit' || m === 'saving';
    p.editor.hidden = !editing;
    p.view.hidden = editing;
    p.editBtn.parentNode.hidden = editing;
    p.keepBtn.disabled = m === 'saving';
    p.cancelBtn.disabled = m === 'saving';
    p.root.setAttribute('data-mode', m);
  }

  function startEdit() {
    if (!p || p.index === null || p.mode === 'saving') { return false; }
    if (p.mode === 'edit') {
      p.input.focus();
      return true;
    }
    var c = cue();
    if (!c) { return false; }
    hideRemember();
    closePop();
    p.editOrig = c.target || '';
    p.input.value = p.editOrig;
    setMode('edit');
    paintMirror();
    scheduleCheck();
    p.input.focus();
    return true;
  }

  function cancelEdit() {
    if (!p || p.mode !== 'edit') { return false; }
    setMode('view');
    var c = cue();
    if (c) { renderTarget(c); }
    p.view.focus();
    return true;
  }

  // Leaving the cue while editing never loses text: a changed text is kept, else cancelled.
  function leaveEditor() {
    var text = String(p.input.value).replace(/\r/g, '');
    var index = p.index;
    setMode('view');
    if (text !== p.editOrig && index !== null) {
      window.VP_Workspace.cmd.edit(index, text).then(null, function (err) { window.VP_App.showError(err); });
    }
  }

  function acceptEdit() {
    var P = window.Promise;
    if (!p || p.mode !== 'edit') { return P.resolve(false); }
    var text = String(p.input.value).replace(/\r/g, '');
    if (text === p.editOrig) {
      cancelEdit();
      return P.resolve(false);
    }
    var index = p.index;
    var gen = p.gen;
    setMode('saving');
    return window.VP_Workspace.cmd.edit(index, text).then(function (r) {
      if (!p || p.gen !== gen) { return true; }
      setMode('view');
      if (p.index !== index) { return true; }
      p.remembered = { index: index, text: r.cue.target };
      var c = cue();
      if (c) { renderTarget(c); }
      p.rememberEl.hidden = false;
      p.mode = 'remember';
      p.root.setAttribute('data-mode', 'remember');
      var first = window.VP_Dom.qs('[data-remember="phrase"]', p.rememberEl);
      if (first) { first.focus(); }
      return true;
    }, function (err) {
      if (p && p.gen === gen) {
        setMode('edit');
        p.input.focus();
      }
      window.VP_App.showError(err);
      return false;
    });
  }

  function hideRemember() {
    if (!p) { return false; }
    var was = !p.rememberEl.hidden;
    p.rememberEl.hidden = true;
    p.remembered = null;
    if (p.mode === 'remember') {
      p.mode = 'view';
      p.root.setAttribute('data-mode', 'view');
    }
    return was;
  }

  function remember(scope) {
    if (!p || p.mode !== 'remember' || !p.remembered) { return false; }
    var r = p.remembered;
    hideRemember();
    p.view.focus();
    if (scope === 'phrase') {
      window.VP_Workspace.cmd.edit(r.index, r.text, 'phrase').then(function () {
        window.VP_Toast.show({ key: 'target.remember.done.label', kind: 'success' });
      }, function (err) { window.VP_App.showError(err); });
    }
    return true;
  }

  function scheduleCheck() {
    if (p.checkTimer !== null) { window.VP_Timers.clear(p.checkTimer); }
    p.checkTimer = window.VP_Timers.setTimeout(OWNER, function () {
      p.checkTimer = null;
      runCheck();
    }, CHECK_MS);
  }

  function wordKey(word) { return p.langs.dst + '|' + word.toLowerCase(); }

  function runCheck() {
    if (!p || p.mode !== 'edit' || !CHECK_LANGS[p.langs.dst]) { return; }
    var gen = p.gen;
    var words = String(p.input.value).match(new RegExp(WORD_RE.source, 'g')) || [];
    var asked = {};
    var todo = [];
    for (var i = 0; i < words.length && todo.length < 40; i++) {
      var k = wordKey(words[i]);
      if (asked[k] || p.words.get(k) !== undefined) { continue; }
      asked[k] = true;
      todo.push(words[i]);
    }
    todo.forEach(function (word) {
      window.VP_Bridge.call('word.inspect', { text: word, lang: p.langs.dst }).then(function (r) {
        if (!p || p.gen !== gen) { return; }
        p.words.put(wordKey(word), r);
        if (p.mode === 'edit') { paintMirror(); }
      }, function () { return null; });
    });
    paintMirror();
  }

  function paintMirror() {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var text = String(p.input.value);
    D.clear(p.mirror);
    var re = new RegExp(WORD_RE.source, 'g');
    var pos = 0;
    var unknown = [];
    var m;
    var check = CHECK_LANGS[p.langs.dst] === true;
    while ((m = re.exec(text)) !== null) {
      var r = check ? p.words.get(wordKey(m[0])) : undefined;
      var bad = !!(r && r.analyses && !r.analyses.length);
      if (!bad) { continue; }
      if (m.index > pos) { p.mirror.appendChild(document.createTextNode(text.slice(pos, m.index))); }
      p.mirror.appendChild(D.el('mark', { className: 'vp-unknown', text: m[0] }));
      pos = m.index + m[0].length;
      if (unknown.length < 5) { unknown.push({ word: m[0], suggestions: (r.suggestions || []).slice(0, 3) }); }
    }
    p.mirror.appendChild(document.createTextNode(text.slice(pos) + '​'));
    D.clear(p.unknownEl);
    if (!unknown.length) {
      p.unknownEl.hidden = true;
      return;
    }
    p.unknownEl.hidden = false;
    for (var u = 0; u < unknown.length; u++) {
      var x = unknown[u];
      p.unknownEl.appendChild(D.el('span', { className: 'vp-unknown-item', text: x.suggestions.length ?
        T.t('target.editor.unknown.suggest.label', { word: x.word, list: x.suggestions.join(', ') }) :
        T.t('target.editor.unknown.label', { word: x.word }) }));
    }
  }

  // ---------------------------------------------------------------- word card
  function closePop() {
    if (!p || p.pop.hidden) { return false; }
    p.pop.hidden = true;
    window.VP_Dom.clear(p.popBody);
    p.popTok = null;
    return true;
  }

  function renderPop(tok, r) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    D.clear(p.popBody);
    var a = r && r.analyses && r.analyses[0];
    if (!r) {
      p.popBody.appendChild(D.el('p', { className: 'vp-hint', text: T.t('target.word.loading.label') }));
      return;
    }
    if (!a) {
      p.popBody.appendChild(D.el('p', { className: 'vp-pop-head vp-text', lang: p.langs.dst, text: tok.text }));
      p.popBody.appendChild(D.el('p', { text: T.t('target.word.unknown.tooltip') }));
      if (r.suggestions && r.suggestions.length) { p.popBody.appendChild(D.el('p', { className: 'vp-hint', text: T.t('target.word.suggest.label', { list: r.suggestions.slice(0, 3).join(', ') }) })); }
      return;
    }
    var l = a.lemma || {};
    var f = a.features || {};
    var gloss = window.VP_I18n.lang() === 'es-MX' ? (l.glossEs || l.glossEn) : (l.glossEn || l.glossEs);
    var form = [term('case', f['case']), term('number', f.number), term('person', f.person), term('tense', f.tense), term('mood', f.mood), term('voice', f.voice)].filter(function (x) { return x; });
    p.popBody.appendChild(D.el('p', { className: 'vp-pop-head' }, [
      D.el('span', { className: 'vp-text vp-pop-lemma', lang: p.langs.dst, text: display(l.head || a.display || tok.text) }),
      l.pos ? D.el('span', { className: 'vp-pop-pos', text: term('pos', l.pos) }) : null,
      l.tier ? D.el('span', { className: 'vp-pop-tier', text: T.t('tier.t' + l.tier + '.label') }) : null
    ]));
    if (gloss) { p.popBody.appendChild(D.el('p', { className: 'vp-pop-gloss', text: T.t('target.word.gloss.label', { gloss: gloss }) })); }
    if (form.length) { p.popBody.appendChild(D.el('p', { className: 'vp-hint', text: T.t('target.word.form.label', { form: form.join(', ') }) })); }
  }

  function openWord(k) {
    if (!p || !p.tokens[k]) { return false; }
    var tok = p.tokens[k];
    var lang = p.langs.dst;
    window.VP_Store.set('inspect', { index: p.index, token: k, text: tok.text, lang: lang, lemmaId: tok.lemmaId === undefined ? null : tok.lemmaId });
    p.popTok = k;
    p.pop.hidden = false;
    var key = wordKey(tok.text);
    var cached = p.words.get(key);
    renderPop(tok, cached || null);
    if (cached === undefined) {
      var gen = p.gen;
      window.VP_Bridge.call('word.inspect', { text: tok.text, lang: lang }).then(function (r) {
        if (!p || p.gen !== gen) { return; }
        p.words.put(key, r);
        if (p.popTok === k && !p.pop.hidden) { renderPop(tok, r); }
      }, function () {
        if (p && p.gen === gen && p.popTok === k) { renderPop(tok, { analyses: [], suggestions: [] }); }
      });
    }
    return true;
  }

  // ---------------------------------------------------------------- source word card and interlinear
  function renderCard(k) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    var t = p.srcToks[k];
    if (!t) { return; }
    var info = srcInfo(k);
    var a = analysisOf(k);
    D.clear(p.srcCard);
    var showEmoji = settings().showEmoji !== false;
    p.srcCard.appendChild(D.el('p', { className: 'vp-pop-head' }, [
      D.el('span', { className: 'vp-text vp-pop-lemma', lang: p.langs.src, text: display(info.unknown ? t.text : info.head) }),
      info.tier && window.VP_Inspector ? window.VP_Inspector.tierBadge(info.tier, true) : null,
      info.emoji && showEmoji ? D.el('span', { className: 'vp-emoji', role: 'img', 'aria-label': info.head, text: info.emoji }) : null
    ]));
    if (info.unknown) {
      p.srcCard.appendChild(D.el('p', { className: 'vp-hint', text: T.t('source.word.unknown.label') }));
      return;
    }
    if (info.words) { p.srcCard.appendChild(D.el('p', { className: 'vp-src-card-form', text: T.t('target.word.form.label', { form: info.words }) })); }
    if (info.gloss) { p.srcCard.appendChild(D.el('p', { className: 'vp-pop-gloss', text: T.t('target.word.gloss.label', { gloss: info.gloss }) })); }
    var role = roleText(a && a.role);
    if (role) { p.srcCard.appendChild(D.el('p', { className: 'vp-hint', text: T.t('source.word.role.label', { role: role }) })); }
  }

  function showCard(k) {
    if (!p || !p.srcToks[k]) { return false; }
    p.cardTok = k;
    fetchSrcWord(k);
    renderCard(k);
    p.srcCard.hidden = false;
    return true;
  }

  function hideCard() {
    if (!p || p.cardTok === null) { return false; }
    p.cardTok = null;
    p.srcCard.hidden = true;
    window.VP_Dom.clear(p.srcCard);
    return true;
  }

  function renderIlLine() {
    var c = cue();
    if (c && p.mode !== 'empty') { renderSource(c); }
  }

  // Ctrl+I: null when the pair is not read from Latin/Greek (the key goes on), else the state.
  function toggleInterlinear(on) {
    if (!p || p.index === null || !isReading(cue())) { return null; }
    var want = on === undefined ? !interlinearOn() : !!on;
    window.VP_Store.patch('settings', { interlinear: want });
    if (window.VP_App && typeof window.VP_App.saveSettings === 'function') { window.VP_App.saveSettings({ interlinear: want }); }
    window.VP_Toast.show({ key: want ? 'source.interlinear.on.label' : 'source.interlinear.off.label' });
    return want;
  }

  // A source word of a reading pair: the Word tab explains how the engine read it.
  function openSourceWord(k) {
    if (!p || !p.srcToks[k]) { return false; }
    var t = p.srcToks[k];
    window.VP_Store.set('inspect', { index: p.index, token: k, text: t.text, lang: p.langs.src, lemmaId: t.lemmaId === undefined ? null : t.lemmaId, side: 'analysis' });
    return true;
  }

  function onSrcOver(e) {
    var chip = window.VP_Dom.closest(e.target, '.vp-src-word', p.srcText);
    if (chip) { showCard(Number(chip.getAttribute('data-src-tok'))); }
  }

  function onSrcOut(e) {
    var chip = window.VP_Dom.closest(e.target, '.vp-src-word', p.srcText);
    var to = e.relatedTarget && e.relatedTarget.nodeType === 1 ? window.VP_Dom.closest(e.relatedTarget, '.vp-src-word', p.srcText) : null;
    if (chip && to !== chip) { hideCard(); }
  }

  function onSrcClick(e) {
    var chip = window.VP_Dom.closest(e.target, '.vp-src-word', p.srcText);
    if (chip) { openSourceWord(Number(chip.getAttribute('data-src-tok'))); }
  }

  function onSrcKey(e) {
    var chip = window.VP_Dom.closest(e.target, '.vp-src-word', p.srcText);
    if (!chip || e.ctrlKey || e.altKey || e.metaKey) { return; }
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
      e.preventDefault();
      e.stopPropagation();
      openSourceWord(Number(chip.getAttribute('data-src-tok')));
    }
  }

  function chooseAlt(n) {
    if (!p || p.mode !== 'view' || p.index === null || !p.alts[n - 1]) { return false; }
    var index = p.index;
    var alt = p.alts[n - 1];
    var gen = p.gen;
    window.VP_Workspace.cmd.choose(index, alt.engineIndex).then(function () {
      if (!p || p.gen !== gen) { return; }
      p.details.drop(index);
      if (p.index === index) {
        p.detail = null;
        fetchDetail(index);
      }
    }, function (err) { window.VP_App.showError(err); });
    return true;
  }

  // ---------------------------------------------------------------- events
  function onViewClick(e) {
    var word = window.VP_Dom.closest(e.target, '.vp-word', p.view);
    if (word) {
      openWord(Number(word.getAttribute('data-tok')));
      return;
    }
    if (p.mode === 'view' || p.mode === 'remember') { startEdit(); }
  }

  function onViewKey(e) {
    var word = window.VP_Dom.closest(e.target, '.vp-word', p.view);
    if (!word || e.ctrlKey || e.altKey || e.metaKey) { return; }
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
      e.preventDefault();
      e.stopPropagation();
      openWord(Number(word.getAttribute('data-tok')));
    }
  }

  function onAction(e, btn) {
    var a = btn.getAttribute('data-pane-action');
    if (a === 'edit') { startEdit(); } else if (a === 'cancel') { cancelEdit(); } else if (a === 'keep') { acceptEdit(); } else if (a === 'accept') {
      if (p.index !== null) { window.VP_Workspace.cmd.review([p.index], true, { labelKey: 'workspace.history.accept.label' }).then(null, function (err) { window.VP_App.showError(err); }); }
    } else if (a === 'closePop') {
      closePop();
      p.view.focus();
    } else if (a === 'interlinear') {
      toggleInterlinear();
    }
  }

  function build(el) {
    var D = window.VP_Dom;
    var T = window.VP_I18n;
    p.srcText = D.el('div', { id: 'vp-src-text', className: 'vp-pane-text vp-text', lang: p.langs.src });
    p.srcCard = D.el('div', { id: 'vp-src-card', className: 'vp-src-card', role: 'tooltip', hidden: true });
    p.ilBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary vp-il-btn', 'data-pane-action': 'interlinear', 'aria-pressed': 'false', 'aria-keyshortcuts': 'Control+I', hidden: true, 'data-i18n': 'source.interlinear.cta', 'data-i18n-title': 'source.interlinear.tooltip' });
    p.timing = D.el('span', { className: 'vp-timing vp-mono' });
    p.dur = D.el('span', { className: 'vp-dur vp-mono' });
    p.cps = D.el('span', { className: 'vp-cps' });
    p.tgtChip = D.el('span', { className: 'vp-chip', hidden: true }, [D.el('span', { className: 'vp-chip-shape', 'aria-hidden': 'true' }), D.el('span')]);
    p.tgtState = D.el('span', { className: 'vp-tgt-state', hidden: true });
    p.editBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-pane-action': 'edit', 'aria-keyshortcuts': 'E', 'data-i18n': 'target.edit.cta' });
    p.acceptBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-pane-action': 'accept', 'aria-keyshortcuts': 'Enter', 'data-i18n': 'target.accept.cta' });
    p.view = D.el('div', { id: 'vp-target-view', className: 'vp-pane-text vp-target-view vp-text', lang: p.langs.dst, tabIndex: -1, 'aria-labelledby': 'vp-tgt-title' });
    p.mirror = D.el('div', { className: 'vp-editor-mirror vp-text', 'aria-hidden': 'true', lang: p.langs.dst });
    p.input = D.el('textarea', { id: 'vp-editor', className: 'vp-editor-input vp-text', lang: p.langs.dst, rows: '3', spellcheck: 'false', 'aria-describedby': 'vp-editor-help' });
    p.unknownEl = D.el('p', { className: 'vp-editor-unknown', 'aria-live': 'polite', hidden: true });
    p.keepBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', 'data-pane-action': 'keep', 'aria-keyshortcuts': 'Control+Enter', 'data-i18n': 'target.editor.keep.cta' });
    p.cancelBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-pane-action': 'cancel', 'aria-keyshortcuts': 'Escape', 'data-i18n': 'target.editor.cancel.cta' });
    p.editor = D.el('div', { className: 'vp-editor', hidden: true }, [
      D.el('label', { htmlFor: 'vp-editor', className: 'vp-visually-hidden', 'data-i18n': 'target.editor.aria' }),
      D.el('div', { className: 'vp-editor-wrap' }, [p.mirror, p.input]),
      D.el('p', { id: 'vp-editor-help', className: 'vp-hint', 'data-i18n': 'target.editor.hint' }),
      p.unknownEl,
      D.el('div', { className: 'vp-row vp-editor-actions' }, [p.cancelBtn, p.keepBtn])
    ]);
    p.rememberEl = D.el('div', { className: 'vp-remember', role: 'group', 'aria-labelledby': 'vp-remember-q', hidden: true }, [
      D.el('span', { id: 'vp-remember-q', 'data-i18n': 'target.remember.label' }),
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-remember': 'phrase', 'data-i18n': 'target.remember.phrase.cta' }),
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-remember': 'cue', 'data-i18n': 'target.remember.cue.cta' })
    ]);
    p.popBody = D.el('div', { className: 'vp-pop-body' });
    p.pop = D.el('div', { className: 'vp-word-pop', role: 'region', 'aria-live': 'polite', 'data-i18n-aria': 'target.word.aria', hidden: true }, [
      p.popBody,
      D.el('button', { type: 'button', className: 'vp-btn vp-btn-icon vp-pop-close', 'data-pane-action': 'closePop', 'data-i18n-aria': 'dialog.close.aria' }, [
        D.el('svg', { className: 'vp-icon', 'aria-hidden': 'true', focusable: 'false' }, [D.el('use', { href: '#vp-i-close' })])
      ])
    ]);
    p.previewLines = D.el('div', { className: 'vp-preview-guide vp-text' });
    p.previewWarn = D.el('div', { className: 'vp-preview-warn', 'aria-live': 'polite' });
    p.altList = D.el('ol', { className: 'vp-alt-list' });
    p.altEmpty = D.el('p', { className: 'vp-hint vp-alt-empty' });
    var subs = p.kind !== 'text';
    p.body = D.el('div', { className: 'vp-panes-body', hidden: true }, [
      D.el('section', { className: 'vp-pane vp-pane-source', 'aria-labelledby': 'vp-src-title' }, [
        D.el('div', { className: 'vp-pane-head' }, [
          D.el('h2', { id: 'vp-src-title', className: 'vp-pane-title', 'data-i18n': 'source.pane.title', 'data-i18n-vars': { lang: langName(p.langs.src) } }),
          subs ? p.timing : null, subs ? p.dur : null, subs ? p.cps : null, p.ilBtn
        ]),
        p.srcText,
        p.srcCard
      ]),
      D.el('section', { className: 'vp-pane vp-pane-target', 'aria-labelledby': 'vp-tgt-title' }, [
        D.el('div', { className: 'vp-pane-head' }, [
          D.el('h2', { id: 'vp-tgt-title', className: 'vp-pane-title', 'data-i18n': 'target.pane.title', 'data-i18n-vars': { lang: langName(p.langs.dst) } }),
          p.tgtChip, p.tgtState
        ]),
        p.view,
        D.el('div', { className: 'vp-row vp-target-actions' }, [p.editBtn, p.acceptBtn]),
        p.editor, p.rememberEl, p.pop
      ]),
      subs ? D.el('section', { className: 'vp-preview', 'aria-labelledby': 'vp-preview-title' }, [
        D.el('h3', { id: 'vp-preview-title', 'data-i18n': 'target.preview.title' }),
        D.el('div', { className: 'vp-preview-screen' }, [p.previewLines]),
        p.previewWarn
      ]) : null,
      D.el('section', { className: 'vp-alts', 'aria-labelledby': 'vp-alts-title' }, [
        D.el('h3', { id: 'vp-alts-title', 'data-i18n': 'target.alt.title' }),
        p.altList, p.altEmpty
      ])
    ]);
    p.emptyEl = D.el('p', { className: 'vp-panes-empty vp-hint', 'data-i18n': 'target.select.empty' });
    p.root = D.el('div', { className: 'vp-panes', 'data-mode': 'empty', 'data-kind': p.kind }, [p.emptyEl, p.body]);
    el.appendChild(p.root);
    T.bind(p.root);
  }

  function mount(el, opts) {
    if (p) { destroy(); }
    opts = opts || {};
    var pair = String(opts.pair || 'en-la').split('-');
    p = {
      gen: (mount.gen = (mount.gen || 0) + 1), kind: opts.kind || 'subs', langs: { src: pair[0], dst: pair[1] || pair[0] },
      mode: 'empty', index: null, detail: null, shown: null, tokens: [], alts: [], details: lru(DETAIL_CAP), words: lru(WORD_CAP),
      editOrig: '', remembered: null, checkTimer: null, popTok: null, removers: [], srcToks: [], cardTok: null, asked: {}, reading: false
    };
    p.reading = CLASSICAL[p.langs.src] === true && MODERN[p.langs.dst] === true;
    build(el);
    var D = window.VP_Dom;
    D.on(p.view, 'click', onViewClick, { owner: OWNER });
    D.on(p.view, 'keydown', onViewKey, { owner: OWNER });
    D.on(p.srcText, 'mouseover', onSrcOver, { owner: OWNER });
    D.on(p.srcText, 'mouseout', onSrcOut, { owner: OWNER });
    D.on(p.srcText, 'focusin', onSrcOver, { owner: OWNER });
    D.on(p.srcText, 'focusout', onSrcOut, { owner: OWNER });
    D.on(p.srcText, 'click', onSrcClick, { owner: OWNER });
    D.on(p.srcText, 'keydown', onSrcKey, { owner: OWNER });
    D.delegate(p.root, '[data-pane-action]', 'click', onAction, { owner: OWNER });
    D.delegate(p.altList, '[data-alt]', 'click', function (e, b) { chooseAlt(Number(b.getAttribute('data-alt'))); }, { owner: OWNER });
    D.delegate(p.rememberEl, '[data-remember]', 'click', function (e, b) { remember(b.getAttribute('data-remember')); }, { owner: OWNER });
    D.on(p.input, 'input', function () {
      paintMirror();
      scheduleCheck();
    }, { owner: OWNER });
    D.on(p.input, 'scroll', function () { p.mirror.scrollTop = p.input.scrollTop; }, { owner: OWNER });
    var S = window.VP_Store;
    p.removers.push(S.subscribe('selection', function (sel) { show(sel ? sel.index : null); }));
    p.removers.push(S.subscribe('cues', onCuesChanged));
    p.removers.push(S.subscribe('settings', function () {
      p.shown = null;
      render();
    }));
    p.removers.push(window.VP_I18n.onLanguageChanged(function () {
      var title = D.qs('#vp-src-title', p.root);
      if (title) { title.setAttribute('data-i18n-vars', JSON.stringify({ lang: langName(p.langs.src) })); }
      title = D.qs('#vp-tgt-title', p.root);
      if (title) { title.setAttribute('data-i18n-vars', JSON.stringify({ lang: langName(p.langs.dst) })); }
      window.VP_I18n.bind(p.root);
      p.shown = null;
      render();
      if (p.popTok !== null && p.tokens[p.popTok]) { renderPop(p.tokens[p.popTok], p.words.get(wordKey(p.tokens[p.popTok].text)) || null); }
      if (p.cardTok !== null) { renderCard(p.cardTok); }
    }));
    p.removers.push(window.VP_Debug.registerCache('panesCueGet', function () { return p ? p.details.size() : 0; }));
    p.removers.push(window.VP_Debug.registerCache('panesWords', function () { return p ? p.words.size() : 0; }));
    var sel = S.get('selection');
    show(sel ? sel.index : null);
  }

  function destroy() {
    if (!p) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (p.removers.length) { p.removers.pop()(); }
    p.details.clear();
    p.words.clear();
    p.srcToks = [];
    if (p.root && p.root.parentNode) { p.root.parentNode.removeChild(p.root); }
    p = null;
  }

  function i18nKeys() {
    var keys = ['source.cps.label', 'source.cps.fast.label', 'source.cps.tooltip', 'unit.secondsShort', 'target.editor.unknown.label',
      'target.editor.unknown.suggest.label', 'target.word.gloss.label', 'target.word.form.label', 'target.word.suggest.label', 'target.word.loading.label',
      'target.preview.overflow.label', 'target.preview.fast.label', 'target.alt.none.label', 'target.alt.loading.label', 'target.state.reviewed.label',
      'target.state.edited.label', 'target.state.stale.label', 'target.state.new.label', 'source.word.unknown.label', 'source.word.role.label',
      'source.interlinear.on.label', 'source.interlinear.off.label'];
    ['subject', 'object', 'verb', 'indirectObject', 'predicate', 'vocative', 'genitive', 'ablative', 'ablativeAbsolute', 'place', 'placeTo', 'prepositionalPhrase',
      'preposition', 'adverb', 'connector', 'negation', 'questionWord', 'comparison', 'infinitive', 'numeral', 'interjection'].forEach(function (r) { keys.push('source.word.role.' + r + '.label'); });
    return keys;
  }

  window.VP_Panes = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return p !== null; },
    mode: function () { return p ? p.mode : 'empty'; },
    index: function () { return p ? p.index : null; },
    startEdit: startEdit,
    cancelEdit: cancelEdit,
    acceptEdit: acceptEdit,
    remember: remember,
    chooseAlt: chooseAlt,
    openWord: openWord,
    dismiss: function () { return hideRemember() || closePop() || hideCard(); },
    tokens: function () { return p ? p.tokens.slice() : []; },
    alternatives: function () { return p ? p.alts.slice() : []; },
    setEditorText: function (text) {
      if (!p || p.mode !== 'edit') { return false; }
      p.input.value = text;
      paintMirror();
      scheduleCheck();
      return true;
    },
    renderMarkup: renderMarkup,
    reading: function () { return !!p && isReading(cue()); },
    sourceTokens: function () { return p ? p.srcToks.slice() : []; },
    openSourceWord: openSourceWord,
    showCard: showCard,
    hideCard: hideCard,
    toggleInterlinear: toggleInterlinear,
    interlinear: interlinearOn,
    reasonKey: reasonKey,
    roleText: roleText,
    stats: function () { return p ? { mode: p.mode, index: p.index, details: p.details.size(), words: p.words.size(), tokens: p.tokens.length, sourceTokens: p.srcToks.length } : null; },
    i18nKeys: i18nKeys
  };
}());
