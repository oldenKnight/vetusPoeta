/* vp_debug.js - test hooks (PREDESIGN 6.3). Enabled by ?debug=1, by localStorage
 * "vp.debug" = "1", or by VP_Debug.enable(true); otherwise stats() returns null and the
 * router skips its leak checks. Release builds keep it disabled.
 *
 * VP_Debug.stats() -> {listeners, timers, domNodes, cueRows, caches:{name: size}}
 * VP_Debug.assert(cond, code, detail), fail(code, detail), failures(), clearFailures()
 * VP_Debug.registerCache(name, sizeFn) -> remover (screens report their caches)
 */
(function () {
  'use strict';

  var MAX_FAILURES = 100;
  var flag = null;
  var failures = [];
  var caches = {};

  function enabled() {
    if (flag !== null) { return flag; }
    var on = /[?&]debug=1(&|$)/.test(String(window.location && window.location.search));
    if (!on) {
      try {
        on = window.localStorage.getItem('vp.debug') === '1';
      } catch (e) {
        on = false;
      }
    }
    flag = on;
    return flag;
  }

  function size(mod, fn) {
    var m = window[mod];
    return m && typeof m[fn] === 'function' ? m[fn]() : 0;
  }

  function stats() {
    if (!enabled()) { return null; }
    var c = {
      cues: size('VP_Store', 'cueCount'),
      history: size('VP_History', 'size'),
      toasts: size('VP_Toast', 'count'),
      dialogs: size('VP_Dialog', 'count'),
      bridgePending: window.VP_Bridge ? window.VP_Bridge.stats().pending : 0,
      bridgeHandlers: size('VP_Bridge', 'handlerCount'),
      i18nListeners: size('VP_I18n', 'listenerCount'),
      storeSubscribers: size('VP_Store', 'subscriberCount'),
      keyHandlers: size('VP_Keys', 'handlerCount')
    };
    var names = Object.keys(caches);
    for (var i = 0; i < names.length; i++) { c[names[i]] = caches[names[i]](); }
    return {
      listeners: size('VP_Dom', 'count'),
      timers: size('VP_Timers', 'count'),
      domNodes: document.getElementsByTagName('*').length,
      cueRows: document.getElementsByClassName('vp-cue-row').length,
      caches: c
    };
  }

  function fail(code, detail) {
    if (failures.length < MAX_FAILURES) { failures.push({ code: code, detail: detail || null }); }
    if (window.console) { window.console.error('[VP_Debug] ' + code + ' ' + JSON.stringify(detail || {})); }
  }

  function assert(cond, code, detail) {
    if (!cond) { fail(code, detail); }
    return !!cond;
  }

  window.VP_Debug = {
    enabled: enabled,
    enable: function (on) { flag = !!on; },
    stats: stats,
    assert: assert,
    fail: fail,
    failures: function () { return failures.slice(); },
    clearFailures: function () { failures = []; },
    registerCache: function (name, fn) {
      caches[name] = fn;
      return function () { if (caches[name] === fn) { delete caches[name]; } };
    }
  };
}());
