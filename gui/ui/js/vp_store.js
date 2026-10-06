/* vp_store.js - app state: settings, engine status, project manifest, and a capped window
 * of cue records (PREDESIGN 6.2: never more than 50,000 cue records in JS).
 *
 * VP_Store.get(key) / set(key, value) / patch(key, partial)  top-level keys; get also reads
 *   dotted paths ('settings.theme'); set notifies subscribers of that key and of '*'
 * VP_Store.subscribe(key, fn) -> remover; unsubscribe(key, fn); subscriberCount()
 * VP_Store.putCues(list), getCue(index), cueCount(), setCueTotal(n), cueTotal(), clearCues()
 *   the oldest records are dropped first when the cap is passed
 * VP_Store.reset()   on project close: nulls the large arrays and the project state
 */
(function () {
  'use strict';

  var CUE_CAP = 50000;

  var state = {};
  var subs = {};
  var cues = null;
  var order = null;
  var head = 0;
  var records = 0;
  var total = 0;
  var evicted = 0;

  function get(key) {
    if (Object.prototype.hasOwnProperty.call(state, key)) { return state[key]; }
    var parts = String(key).split('.');
    var v = state;
    for (var i = 0; i < parts.length; i++) {
      if (v === null || v === undefined || typeof v !== 'object') { return undefined; }
      v = v[parts[i]];
    }
    return v;
  }

  function notify(key, value) {
    var lists = [subs[key], subs['*']];
    for (var l = 0; l < lists.length; l++) {
      if (!lists[l]) { continue; }
      var copy = lists[l].slice();
      for (var i = 0; i < copy.length; i++) {
        if (lists[l].indexOf(copy[i]) < 0) { continue; }
        try {
          copy[i](value, key);
        } catch (e) {
          if (window.console) { window.console.error('[VP_Store] subscriber of ' + key + ' failed', e); }
        }
      }
    }
  }

  function set(key, value) {
    state[key] = value;
    notify(key, value);
    return value;
  }

  function patch(key, partial) {
    var cur = state[key];
    var next = {};
    var k;
    if (cur && typeof cur === 'object') { for (k in cur) { if (Object.prototype.hasOwnProperty.call(cur, k)) { next[k] = cur[k]; } } }
    for (k in partial) { if (Object.prototype.hasOwnProperty.call(partial, k)) { next[k] = partial[k]; } }
    return set(key, next);
  }

  function subscribe(key, fn) {
    if (typeof fn !== 'function') { throw new Error('VP_Store.subscribe: not a function'); }
    if (!subs[key]) { subs[key] = []; }
    subs[key].push(fn);
    return function () { unsubscribe(key, fn); };
  }

  function unsubscribe(key, fn) {
    var list = subs[key];
    if (!list) { return false; }
    var i = list.indexOf(fn);
    if (i < 0) { return false; }
    list.splice(i, 1);
    if (!list.length) { delete subs[key]; }
    return true;
  }

  function subscriberCount() {
    var n = 0;
    var keys = Object.keys(subs);
    for (var i = 0; i < keys.length; i++) { n += subs[keys[i]].length; }
    return n;
  }

  function ensureCues() {
    if (cues === null) {
      cues = {};
      order = [];
      head = 0;
      records = 0;
    }
  }

  function evict() {
    while (records > CUE_CAP) {
      var idx = order[head];
      order[head] = undefined;
      head++;
      if (cues[idx] !== undefined) {
        delete cues[idx];
        records--;
        evicted++;
      }
    }
    if (head > 4096 && head * 2 > order.length) {
      order = order.slice(head);
      head = 0;
    }
  }

  function putCues(list) {
    ensureCues();
    for (var i = 0; i < list.length; i++) {
      var c = list[i];
      if (!c || typeof c.index !== 'number') { continue; }
      if (cues[c.index] === undefined) {
        order.push(c.index);
        records++;
      }
      cues[c.index] = c;
    }
    evict();
    notify('cues', records);
    return records;
  }

  function getCue(index) {
    return cues !== null && cues[index] !== undefined ? cues[index] : null;
  }

  function clearCues() {
    cues = null;
    order = null;
    head = 0;
    records = 0;
    notify('cues', 0);
  }

  function reset() {
    clearCues();
    total = 0;
    var keys = Object.keys(state);
    for (var i = 0; i < keys.length; i++) {
      if (keys[i] === 'project' || keys[i] === 'job') { set(keys[i], null); }
    }
  }

  window.VP_Store = {
    get: get,
    set: set,
    patch: patch,
    subscribe: subscribe,
    unsubscribe: unsubscribe,
    subscriberCount: subscriberCount,
    putCues: putCues,
    getCue: getCue,
    cueCount: function () { return records; },
    cueCap: function () { return CUE_CAP; },
    setCueTotal: function (n) { total = n; },
    cueTotal: function () { return total; },
    clearCues: clearCues,
    reset: reset,
    stats: function () { return { cueRecords: records, cueOrderLength: order === null ? 0 : order.length - head, evicted: evicted, subscribers: subscriberCount(), arraysNull: cues === null && order === null }; }
  };
}());
