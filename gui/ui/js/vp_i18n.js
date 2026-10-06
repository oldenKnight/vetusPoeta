/* vp_i18n.js - string tables (PREDESIGN 5). No sentence is ever built by concatenation.
 *
 * VP_I18n.load(lang, dict)        register a flat {key: text} table
 * VP_I18n.setLang(lang) -> bool   switch instantly: <html lang>, re-bind the page, notify
 * VP_I18n.t(key, vars)            {name} placeholders; vars.n picks key.zero/.one/.other
 * VP_I18n.num(n, decimals?)       1,019 and 3.5 (en-US and es-MX use the same separators)
 * VP_I18n.bind(root)              fills data-i18n (text), data-i18n-title, -placeholder,
 *                                 -aria (aria-label); data-i18n-vars holds JSON vars
 * VP_I18n.onLanguageChanged(fn)   -> remover; offLanguageChanged(fn); listenerCount()
 * Missing keys fall back to en-US, then to the key itself (and are recorded in missing()).
 */
(function () {
  'use strict';

  var FALLBACK = 'en-US';
  var SEPARATORS = { 'en-US': [',', '.'], 'es-MX': [',', '.'] };
  var ATTRS = [
    ['data-i18n', null],
    ['data-i18n-title', 'title'],
    ['data-i18n-placeholder', 'placeholder'],
    ['data-i18n-aria', 'aria-label']
  ];

  var dicts = {};
  var lang = FALLBACK;
  var listeners = [];
  var missingKeys = {};

  function has(key) {
    return lookup(key) !== null;
  }

  function lookup(key) {
    var d = dicts[lang];
    if (d && Object.prototype.hasOwnProperty.call(d, key)) { return d[key]; }
    d = dicts[FALLBACK];
    if (d && Object.prototype.hasOwnProperty.call(d, key)) { return d[key]; }
    return null;
  }

  function pluralCategory(n) {
    return Math.abs(n) === 1 ? 'one' : 'other';
  }

  function num(n, decimals) {
    if (typeof n !== 'number' || !isFinite(n)) { return String(n); }
    var sep = SEPARATORS[lang] || SEPARATORS[FALLBACK];
    var neg = n < 0;
    var text;
    if (decimals === undefined) {
      text = Math.abs(n) % 1 === 0 ? Math.abs(n).toFixed(0) : Math.abs(n).toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
    } else {
      text = Math.abs(n).toFixed(decimals);
    }
    var parts = text.split('.');
    var whole = parts[0].replace(/\B(?=(\d{3})+(?!\d))/g, sep[0]);
    return (neg ? '-' : '') + whole + (parts.length > 1 ? sep[1] + parts[1] : '');
  }

  function format(text, vars) {
    if (!vars) { return text; }
    return text.replace(/\{(\w+)\}/g, function (whole, name) {
      if (!Object.prototype.hasOwnProperty.call(vars, name)) { return whole; }
      var v = vars[name];
      return typeof v === 'number' ? num(v) : String(v);
    });
  }

  function t(key, vars) {
    var k = key;
    if (vars && typeof vars.n === 'number') {
      if (vars.n === 0 && lookup(key + '.zero') !== null) {
        k = key + '.zero';
      } else if (lookup(key + '.' + pluralCategory(vars.n)) !== null) {
        k = key + '.' + pluralCategory(vars.n);
      }
    }
    var text = lookup(k);
    if (text === null) {
      missingKeys[k] = true;
      return k;
    }
    return format(text, vars);
  }

  function load(code, dict) {
    if (!code || !dict || typeof dict !== 'object') { throw new Error('VP_I18n.load: bad arguments'); }
    dicts[code] = dict;
  }

  function varsOf(node) {
    var raw = node.getAttribute('data-i18n-vars');
    if (!raw) { return null; }
    try {
      return JSON.parse(raw);
    } catch (e) {
      return null;
    }
  }

  function bindNode(node) {
    for (var a = 0; a < ATTRS.length; a++) {
      var key = node.getAttribute(ATTRS[a][0]);
      if (!key) { continue; }
      var text = t(key, varsOf(node));
      if (ATTRS[a][1] === null) {
        if (node.textContent !== text) { node.textContent = text; }
      } else {
        node.setAttribute(ATTRS[a][1], text);
      }
    }
  }

  function bind(root) {
    root = root || document.documentElement;
    if (root.nodeType === 1) { bindNode(root); }
    var list = root.querySelectorAll('[data-i18n], [data-i18n-title], [data-i18n-placeholder], [data-i18n-aria]');
    for (var i = 0; i < list.length; i++) { bindNode(list[i]); }
    return root;
  }

  function setLang(code) {
    if (!dicts[code]) { return false; }
    lang = code;
    document.documentElement.setAttribute('lang', code);
    bind(document.documentElement);
    var copy = listeners.slice();
    for (var i = 0; i < copy.length; i++) {
      if (listeners.indexOf(copy[i]) < 0) { continue; }
      try {
        copy[i](code);
      } catch (e) {
        if (window.console) { window.console.error('[VP_I18n] language listener failed', e); }
      }
    }
    return true;
  }

  function onLanguageChanged(fn) {
    if (typeof fn !== 'function') { throw new Error('VP_I18n.onLanguageChanged: not a function'); }
    listeners.push(fn);
    return function () { offLanguageChanged(fn); };
  }

  function offLanguageChanged(fn) {
    var i = listeners.indexOf(fn);
    if (i >= 0) { listeners.splice(i, 1); }
    return i >= 0;
  }

  window.VP_I18n = {
    load: load,
    t: t,
    num: num,
    has: has,
    bind: bind,
    setLang: setLang,
    lang: function () { return lang; },
    languages: function () { return Object.keys(dicts); },
    onLanguageChanged: onLanguageChanged,
    offLanguageChanged: offLanguageChanged,
    listenerCount: function () { return listeners.length; },
    missing: function () { return Object.keys(missingKeys); }
  };
}());
