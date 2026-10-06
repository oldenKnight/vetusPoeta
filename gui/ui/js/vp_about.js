/* vp_about.js - About and attributions (DESIGN 13, 14; PREDESIGN 1.5). A dialog with tabs:
 * About (wordmark, version from engine.hello, owner credit from i18n), Data sources and
 * licences (the notice text of every lexicon in engine.hello rendered verbatim in a
 * scrollable block, so it can never drift from the data), Software (llama.cpp, miniz,
 * nlohmann/json, doctest: MIT; the WebView2 runtime; the model licence Apache-2.0) and Fonts
 * (Gentium Plus, OFL 1.1, with "Show licence text" that loads fonts/OFL.txt over XHR). Links
 * open through shell.openExternal (http(s) only). The same lines ship as
 * THIRD_PARTY_NOTICES.txt next to the exe (packaging, not this file).
 *
 * VP_About.open(tab?) -> handle; close(); isOpen(); setTab(id); SOFTWARE; FONTS
 */
(function () {
  'use strict';

  var OWNER = 'about';
  var TABS = ['about', 'data', 'software', 'fonts'];
  var SOFTWARE = [
    { name: 'llama.cpp', author: 'Georgi Gerganov and contributors', licence: 'MIT', url: 'https://github.com/ggml-org/llama.cpp' },
    { name: 'miniz', author: 'Rich Geldreich and contributors', licence: 'MIT', url: 'https://github.com/richgel999/miniz' },
    { name: 'nlohmann/json', author: 'Niels Lohmann', licence: 'MIT', url: 'https://github.com/nlohmann/json' },
    { name: 'doctest', author: 'Viktor Kirilov', licence: 'MIT', url: 'https://github.com/doctest/doctest' },
    { name: 'Microsoft Edge WebView2 Runtime', author: 'Microsoft', licence: 'Microsoft Software License Terms (runtime, not redistributed)', url: 'https://developer.microsoft.com/microsoft-edge/webview2/' },
    { name: 'Local language model (optional file)', author: 'its authors, see the model card', licence: 'Apache-2.0', url: 'https://www.apache.org/licenses/LICENSE-2.0' }
  ];
  var FONTS = [
    { name: 'Gentium Plus', author: 'SIL International', licence: 'SIL Open Font License 1.1', url: 'https://software.sil.org/gentium/', file: 'fonts/OFL.txt' }
  ];
  var d = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function hello() {
    var e = window.VP_Store.get('engine') || {};
    return e.hello || {};
  }

  function link(url, text) {
    return el('button', { type: 'button', className: 'vp-link', dataset: { aboutUrl: url }, text: text || url, title: url });
  }

  function entry(x, extra) {
    return el('li', { className: 'vp-about-entry' }, [
      el('span', { className: 'vp-about-name', text: x.name }),
      el('span', { className: 'vp-hint', text: T('about.entry.by.label', { author: x.author }) }),
      el('span', { className: 'vp-about-licence', text: x.licence }),
      link(x.url, T('about.entry.link.cta')),
      extra || null
    ]);
  }

  function aboutTab() {
    var h = hello();
    var kids = [
      el('div', { className: 'vp-about-mark' }, [window.VP_App && typeof window.VP_App.lockup === 'function' ? window.VP_App.lockup() : el('p', { className: 'vp-wordmark', 'data-i18n': 'app.name', text: T('app.name') })]),
      i18nEl('p', 'vp-lead', 'about.tagline.text'),
      el('p', { className: 'vp-hint', text: T('about.version.label', { version: h.version || '?' }) }),
      i18nEl('p', null, 'about.owner.text'),
      i18nEl('p', 'vp-hint', 'about.offline.text')
    ];
    return kids;
  }

  function dataTab() {
    var h = hello();
    var lex = (h.lexicons || []).filter(function (x) { return x && x.notice; });
    var kids = [i18nEl('p', 'vp-hint', 'about.data.hint')];
    if (!lex.length) { kids.push(i18nEl('p', 'vp-hint', 'about.data.none.label')); }
    lex.forEach(function (x) {
      var name = window.VP_I18n.has('app.lexicon.lang.' + x.lang) ? T('app.lexicon.lang.' + x.lang) : x.lang;
      kids.push(el('section', { className: 'vp-about-lex' }, [
        el('h4', { text: T('about.data.lexicon.label', { lang: name, version: x.version || '' }) }),
        el('pre', { className: 'vp-about-notice vp-mono', tabIndex: 0, text: String(x.notice) })
      ]));
    });
    return kids;
  }

  function softwareTab() {
    return [i18nEl('p', 'vp-hint', 'about.software.hint'), el('ul', { className: 'vp-about-list' }, SOFTWARE.map(function (x) { return entry(x); }))];
  }

  function fontsTab() {
    return [i18nEl('p', 'vp-hint', 'about.fonts.hint'), el('ul', { className: 'vp-about-list' }, FONTS.map(function (x) {
      return entry(x, el('div', { className: 'vp-about-licfile' }, [
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'about.licence.show.cta', null, { type: 'button', 'aria-expanded': 'false', dataset: { aboutFile: x.file } }),
        el('pre', { className: 'vp-about-notice vp-mono', hidden: true, tabIndex: 0 })
      ]));
    }))];
  }

  function renderTab() {
    if (!d) { return; }
    var D = window.VP_Dom;
    D.clear(d.body);
    var kids = d.tab === 'about' ? aboutTab() : (d.tab === 'data' ? dataTab() : (d.tab === 'software' ? softwareTab() : fontsTab()));
    D.append(d.body, kids);
    d.body.setAttribute('aria-labelledby', 'vp-about-tab-' + d.tab);
    TABS.forEach(function (t) {
      d.tabs[t].setAttribute('aria-selected', t === d.tab ? 'true' : 'false');
      d.tabs[t].tabIndex = t === d.tab ? 0 : -1;
    });
  }

  function setTab(id, focus) {
    if (!d || TABS.indexOf(id) < 0) { return false; }
    d.tab = id;
    renderTab();
    if (focus) { d.tabs[id].focus(); }
    return true;
  }

  function loadText(url) {
    return new window.Promise(function (resolve, reject) {
      var xhr = new window.XMLHttpRequest();
      xhr.open('GET', url, true);
      if (xhr.overrideMimeType) { xhr.overrideMimeType('text/plain; charset=utf-8'); }
      xhr.onload = function () {
        if (xhr.status !== 200 && xhr.status !== 0) { reject(new Error(url + ': HTTP ' + xhr.status)); } else { resolve(xhr.responseText); }
      };
      xhr.onerror = function () { reject(new Error(url + ': cannot load')); };
      xhr.send();
    });
  }

  function showLicence(btn) {
    var pre = btn.nextSibling;
    if (!pre) { return; }
    var open = pre.hidden;
    pre.hidden = !open;
    btn.setAttribute('aria-expanded', open ? 'true' : 'false');
    btn.setAttribute('data-i18n', open ? 'about.licence.hide.cta' : 'about.licence.show.cta');
    window.VP_I18n.bind(btn);
    if (!open || pre.textContent) { return; }
    pre.textContent = T('about.licence.loading.label');
    loadText(btn.getAttribute('data-about-file')).then(function (text) {
      if (d && pre.parentNode) { pre.textContent = text; }
    }, function () {
      if (d && pre.parentNode) { pre.textContent = T('about.licence.failed.label'); }
    });
  }

  function onClick(e, btn) {
    var url = btn.getAttribute('data-about-url');
    var tab = btn.getAttribute('data-about-tab');
    if (url) {
      window.VP_Bridge.call('shell.openExternal', { url: url }).then(null, function (err) {
        if (err && err.code === 'no_engine' && window.open) { window.open(url, '_blank', 'noopener'); } else { showError(err); }
      });
    } else if (tab) {
      setTab(tab);
    } else if (btn.getAttribute('data-about-file')) {
      showLicence(btn);
    }
  }

  function onKey(e) {
    var tab = window.VP_Dom.closest(e.target, '[data-about-tab]', d.root);
    if (!tab) { return; }
    var i = TABS.indexOf(d.tab);
    var next = null;
    if (e.key === 'ArrowRight') { next = TABS[(i + 1) % TABS.length]; } else if (e.key === 'ArrowLeft') { next = TABS[(i + TABS.length - 1) % TABS.length]; }
    if (next) {
      e.preventDefault();
      setTab(next, true);
    }
  }

  function open(tab) {
    if (d) {
      if (tab) { setTab(tab); }
      return d.handle;
    }
    d = { tab: TABS.indexOf(tab) >= 0 ? tab : 'about', tabs: {}, removers: [], handle: null };
    d.body = el('div', { id: 'vp-about-body', className: 'vp-about-body', role: 'tabpanel' });
    d.root = el('div', { className: 'vp-about' }, [
      el('div', { className: 'vp-tabs', role: 'tablist', 'data-i18n-aria': 'about.tabs.aria', 'aria-label': T('about.tabs.aria') }, TABS.map(function (t) {
        d.tabs[t] = i18nEl('button', 'vp-tab', 'about.tab.' + t + '.label', null, { id: 'vp-about-tab-' + t, type: 'button', role: 'tab', 'aria-selected': 'false', 'aria-controls': 'vp-about-body', tabIndex: -1, dataset: { aboutTab: t } });
        return d.tabs[t];
      })),
      d.body
    ]);
    var D = window.VP_Dom;
    D.delegate(d.root, 'button', 'click', onClick, { owner: OWNER });
    D.on(d.root, 'keydown', onKey, { owner: OWNER });
    d.removers.push(window.VP_I18n.onLanguageChanged(function () { renderTab(); }));
    renderTab();
    d.handle = window.VP_Dialog.open({
      titleKey: 'about.dialog.title', body: d.root, className: 'vp-dialog-wide vp-dialog-about',
      actions: [{ labelKey: 'dialog.close.cta', value: true, kind: 'primary' }],
      initialFocus: '[data-about-tab="' + d.tab + '"]',
      onClose: function () { teardown(); }
    });
    return d.handle;
  }

  function teardown() {
    if (!d) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (d.removers.length) { d.removers.pop()(); }
    d = null;
  }

  function close() {
    if (!d) { return false; }
    var h = d.handle;
    teardown();
    if (h) { h.close(true); }
    return true;
  }

  function i18nKeys() {
    var keys = ['about.version.label', 'about.entry.by.label', 'about.entry.link.cta', 'about.data.lexicon.label', 'about.licence.loading.label', 'about.licence.failed.label', 'about.licence.hide.cta'];
    TABS.forEach(function (t) { keys.push('about.tab.' + t + '.label'); });
    return keys;
  }

  window.VP_About = {
    open: open,
    close: close,
    isOpen: function () { return d !== null; },
    setTab: setTab,
    tab: function () { return d ? d.tab : null; },
    SOFTWARE: SOFTWARE,
    FONTS: FONTS,
    i18nKeys: i18nKeys
  };
}());
