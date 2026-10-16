/* vp_router.js - screen lifecycle (DESIGN 13). A screen is {mount(rootEl, params), destroy()}.
 *
 * VP_Router.register(name, screen); start(rootEl, name, params); go(name, params)
 * start() empties the root (the static loading text); go() always destroys the current
 * screen, empties the root, then mounts the new one.
 * In dev mode (VP_Debug.enabled()) it checks after every destroy() that the listener count
 * (VP_Dom, VP_I18n, VP_Store, VP_Bridge, VP_Keys, VP_History) and the timer count (VP_Timers)
 * are back to their values from just before mount(); a difference is a leak and is recorded
 * with VP_Debug.fail('router.leak', ...).
 * VP_Router.current(), names(), has(name), onChange(fn) -> remover, stop()
 */
(function () {
  'use strict';

  var screens = {};
  var root = null;
  var cur = null;
  var baseline = null;
  var changeFns = [];
  var fallback = 'start';

  function n(obj, fn) {
    var m = window[obj];
    return m && typeof m[fn] === 'function' ? m[fn]() : 0;
  }

  function counts() {
    return {
      listeners: n('VP_Dom', 'count'),
      i18n: n('VP_I18n', 'listenerCount'),
      store: n('VP_Store', 'subscriberCount'),
      bridge: n('VP_Bridge', 'handlerCount'),
      keys: n('VP_Keys', 'handlerCount'),
      history: n('VP_History', 'listenerCount'),
      timers: n('VP_Timers', 'count')
    };
  }

  function debugOn() {
    return !!(window.VP_Debug && window.VP_Debug.enabled());
  }

  function register(name, screen) {
    if (!screen || typeof screen.mount !== 'function' || typeof screen.destroy !== 'function') {
      throw new Error('VP_Router.register: "' + name + '" needs mount() and destroy()');
    }
    screens[name] = screen;
  }

  function leave() {
    if (!cur) { return; }
    var was = cur;
    cur = null;
    try {
      was.screen.destroy();
    } catch (e) {
      if (window.console) { window.console.error('[VP_Router] destroy of ' + was.name + ' failed', e); }
      if (window.VP_Debug) { window.VP_Debug.fail('router.destroy', { screen: was.name, message: String(e && e.message) }); }
    }
    window.VP_Dom.clear(root);
    if (baseline && debugOn()) {
      var after = counts();
      var diff = {};
      var leaked = false;
      var keys = Object.keys(after);
      for (var i = 0; i < keys.length; i++) {
        if (after[keys[i]] !== baseline[keys[i]]) {
          diff[keys[i]] = { before: baseline[keys[i]], after: after[keys[i]] };
          leaked = true;
        }
      }
      window.VP_Debug.assert(!leaked, 'router.leak', { screen: was.name, diff: diff });
    }
    baseline = null;
  }

  function go(name, params) {
    if (!root) { throw new Error('VP_Router.go: call start(rootEl, name) first'); }
    if (!screens[name]) {
      if (window.console) { window.console.warn('[VP_Router] unknown screen "' + name + '", showing "' + fallback + '"'); }
      name = fallback;
      if (!screens[name]) { throw new Error('VP_Router.go: no screen "' + name + '"'); }
    }
    leave();
    baseline = counts();
    cur = { name: name, screen: screens[name], params: params || {} };
    root.setAttribute('data-screen', name);
    try {
      cur.screen.mount(root, cur.params);
      window.VP_I18n.bind(root);
    } catch (e) {
      if (window.console) { window.console.error('[VP_Router] mount of ' + name + ' failed', e); }
      if (window.VP_Debug) { window.VP_Debug.fail('router.mount', { screen: name, message: String(e && e.message) }); }
    }
    var copy = changeFns.slice();
    for (var i = 0; i < copy.length; i++) { copy[i](name); }
    return name;
  }

  function start(rootEl, name, params) {
    if (!rootEl) { throw new Error('VP_Router.start: no root element'); }
    leave();
    root = rootEl;
    window.VP_Dom.clear(root);
    return go(name || fallback, params);
  }

  function stop() {
    leave();
    if (root) { root.removeAttribute('data-screen'); }
    root = null;
  }

  window.VP_Router = {
    register: register,
    start: start,
    go: go,
    stop: stop,
    counts: counts,
    current: function () { return cur ? cur.name : null; },
    has: function (name) { return !!screens[name]; },
    names: function () { return Object.keys(screens); },
    setFallback: function (name) { fallback = name; },
    onChange: function (fn) {
      changeFns.push(fn);
      return function () {
        var i = changeFns.indexOf(fn);
        if (i >= 0) { changeFns.splice(i, 1); }
      };
    }
  };
}());
