/* vp_timers.js - every UI timer has an owner, so a screen's destroy() can clear them all and
 * tests can count what is still alive (PREDESIGN 6.2: no free-running setInterval).
 *
 * VP_Timers.setTimeout(owner, fn, ms) / setInterval(owner, fn, ms) / raf(owner, fn) -> id
 * VP_Timers.clear(id), clearAll(owner) -> number cleared, count(owner?), owners() -> {owner: n}
 */
(function () {
  'use strict';

  var timers = {};
  var live = 0;
  var nextId = 1;

  function check(owner, fn) {
    if (typeof owner !== 'string' || !owner) { throw new Error('VP_Timers: owner must be a non-empty string'); }
    if (typeof fn !== 'function') { throw new Error('VP_Timers: callback must be a function'); }
  }

  function add(owner, kind, handle) {
    timers[handle.id] = { owner: owner, kind: kind, native: handle.native };
    live++;
  }

  function forget(id) {
    if (timers[id]) {
      delete timers[id];
      live--;
    }
  }

  function setTimeoutOwned(owner, fn, ms) {
    check(owner, fn);
    var id = nextId++;
    var nativeId = window.setTimeout(function () {
      forget(id);
      fn();
    }, ms || 0);
    add(owner, 'timeout', { id: id, native: nativeId });
    return id;
  }

  function setIntervalOwned(owner, fn, ms) {
    check(owner, fn);
    var id = nextId++;
    var nativeId = window.setInterval(fn, Math.max(1, ms || 0));
    add(owner, 'interval', { id: id, native: nativeId });
    return id;
  }

  function raf(owner, fn) {
    check(owner, fn);
    var id = nextId++;
    var cb = function (t) {
      forget(id);
      fn(t);
    };
    var nativeId = typeof window.requestAnimationFrame === 'function' ? window.requestAnimationFrame(cb) : window.setTimeout(cb, 16);
    add(owner, typeof window.requestAnimationFrame === 'function' ? 'raf' : 'timeout', { id: id, native: nativeId });
    return id;
  }

  function clear(id) {
    var t = timers[id];
    if (!t) { return false; }
    if (t.kind === 'interval') { window.clearInterval(t.native); } else if (t.kind === 'raf') { window.cancelAnimationFrame(t.native); } else { window.clearTimeout(t.native); }
    forget(id);
    return true;
  }

  function clearAll(owner) {
    var n = 0;
    var ids = Object.keys(timers);
    for (var i = 0; i < ids.length; i++) {
      if (owner === undefined || timers[ids[i]].owner === owner) {
        clear(Number(ids[i]));
        n++;
      }
    }
    return n;
  }

  function count(owner) {
    if (owner === undefined) { return live; }
    var n = 0;
    var ids = Object.keys(timers);
    for (var i = 0; i < ids.length; i++) { if (timers[ids[i]].owner === owner) { n++; } }
    return n;
  }

  function owners() {
    var out = {};
    var ids = Object.keys(timers);
    for (var i = 0; i < ids.length; i++) {
      var o = timers[ids[i]].owner;
      out[o] = (out[o] || 0) + 1;
    }
    return out;
  }

  window.VP_Timers = {
    setTimeout: setTimeoutOwned,
    setInterval: setIntervalOwned,
    raf: raf,
    clear: clear,
    clearAll: clearAll,
    count: count,
    owners: owners
  };
}());
