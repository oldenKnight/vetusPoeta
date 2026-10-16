/* vp_bridge.js - the one door between the UI and the engine (DESIGN 9 framing).
 *
 * Inside the Windows app the shell exposes window.chrome.webview: requests go out as JSON
 * strings {id, cmd, params} through postMessage; responses {id, ok, result|error} and events
 * {event, ...} come back through its 'message' event. In a plain browser with ?mock=1 the
 * transport is VP_MockEngine, which speaks the same JSON. Without either, every call fails
 * with code "no_engine".
 *
 * VP_Bridge.init({mock, timeoutMs, minFlushMs}) -> 'webview' | 'mock' | 'none'
 * VP_Bridge.call(cmd, params, {timeoutMs}) -> Promise(result); rejects with an Error that has
 *   .code (DESIGN 9 code, or the UI-only "timeout" / "no_engine"), .hint, .cmd
 * VP_Bridge.on(event, fn) -> remover; off(event, fn); handlerCount(); stats(); kind()
 * Events are queued and delivered in requestAnimationFrame batches, at most one batch per
 * minFlushMs (100 ms = 10 UI updates per second). Within a batch, translate.cue events of a
 * job are merged into one event whose cues array holds all of them, and translate.progress
 * keeps only the newest. Responses are never delayed.
 */
(function () {
  'use strict';

  var DEFAULT_TIMEOUT_MS = 30000;
  var CMD_TIMEOUT_MS = { 'engine.ping': 2000, 'engine.hello': 10000 };

  var config = { timeoutMs: DEFAULT_TIMEOUT_MS, minFlushMs: 100 };
  var transport = null;
  var kindName = 'none';
  var pending = {};
  var pendingCount = 0;
  var nextId = 1;
  var handlers = {};
  var queue = [];
  var flushScheduled = false;
  var flushTimer = null;
  var flushFrame = null;
  var lastFlush = -1e9;
  var counters = { sent: 0, received: 0, events: 0, flushes: 0, delivered: 0, timeouts: 0 };
  var webviewHandle = null;

  function now() {
    return window.performance && typeof window.performance.now === 'function' ? window.performance.now() : new Date().getTime();
  }

  function makeError(cmd, err) {
    err = err || {};
    var e = new Error(err.message || err.code || 'error');
    e.code = err.code || 'internal';
    e.hint = err.hint || '';
    e.cmd = cmd;
    return e;
  }

  function settle(id, ok, value) {
    var entry = pending[id];
    if (!entry) { return; }
    delete pending[id];
    pendingCount--;
    window.clearTimeout(entry.timer);
    if (ok) { entry.resolve(value); } else { entry.reject(value); }
  }

  function failAll(err) {
    var ids = Object.keys(pending);
    for (var i = 0; i < ids.length; i++) {
      settle(Number(ids[i]), false, makeError(pending[ids[i]].cmd, err));
    }
  }

  function receive(data) {
    var msg = data;
    if (typeof data === 'string') {
      try {
        msg = JSON.parse(data);
      } catch (e) {
        if (window.console) { window.console.warn('[VP_Bridge] ignored a message that is not JSON'); }
        return;
      }
    }
    if (!msg || typeof msg !== 'object') { return; }
    counters.received++;
    if (typeof msg.event === 'string') {
      counters.events++;
      if (msg.event === 'engine.restarted') {
        failAll({ code: 'internal', message: 'engine restarted', hint: '' });
      }
      queue.push(msg);
      scheduleFlush();
      return;
    }
    if (typeof msg.id !== 'number' || !pending[msg.id]) { return; }
    if (msg.ok) {
      settle(msg.id, true, msg.result || {});
    } else {
      settle(msg.id, false, makeError(pending[msg.id].cmd, msg.error));
    }
  }

  function requestFrame(fn) {
    if (typeof window.requestAnimationFrame === 'function') { return { raf: window.requestAnimationFrame(fn) }; }
    return { timer: window.setTimeout(fn, 16) };
  }

  function cancelFrame(f) {
    if (!f) { return; }
    if (f.raf !== undefined) { window.cancelAnimationFrame(f.raf); } else { window.clearTimeout(f.timer); }
  }

  function scheduleFlush() {
    if (flushScheduled) { return; }
    flushScheduled = true;
    var wait = lastFlush + config.minFlushMs - now();
    if (wait > 0) {
      flushTimer = window.setTimeout(function () {
        flushTimer = null;
        flushFrame = requestFrame(flush);
      }, wait);
    } else {
      flushFrame = requestFrame(flush);
    }
  }

  function coalesce(list) {
    var out = [];
    var cueAt = {};
    var progressAt = {};
    for (var i = 0; i < list.length; i++) {
      var m = list[i];
      var job = String(m.jobId);
      if (m.event === 'translate.cue') {
        if (cueAt[job] === undefined) {
          cueAt[job] = out.length;
          out.push({ event: 'translate.cue', jobId: m.jobId, cues: [], batches: 0 });
        }
        var merged = out[cueAt[job]];
        if (Object.prototype.toString.call(m.cues) === '[object Array]') {
          merged.cues = merged.cues.concat(m.cues);
        } else if (m.cue) {
          merged.cues.push(m.cue);
        }
        merged.batches++;
      } else if (m.event === 'translate.progress') {
        if (progressAt[job] === undefined) {
          progressAt[job] = out.length;
          out.push(m);
        } else {
          out[progressAt[job]] = m;
        }
      } else {
        if (m.event === 'translate.done' || m.event === 'translate.error') {
          delete cueAt[job];
          delete progressAt[job];
        }
        out.push(m);
      }
    }
    return out;
  }

  function flush() {
    flushFrame = null;
    flushScheduled = false;
    lastFlush = now();
    counters.flushes++;
    var batch = coalesce(queue);
    queue = [];
    for (var i = 0; i < batch.length; i++) { emit(batch[i].event, batch[i]); }
  }

  function emit(name, payload) {
    var list = handlers[name];
    if (!list || !list.length) { return; }
    list = list.slice();
    for (var i = 0; i < list.length; i++) {
      counters.delivered++;
      try {
        list[i](payload);
      } catch (e) {
        if (window.console) { window.console.error('[VP_Bridge] handler for ' + name + ' failed', e); }
      }
    }
  }

  function call(cmd, params, opts) {
    return new window.Promise(function (resolve, reject) {
      if (!transport) {
        reject(makeError(cmd, { code: 'no_engine', message: 'no engine connected', hint: '' }));
        return;
      }
      var id = nextId++;
      var ms = (opts && opts.timeoutMs) || CMD_TIMEOUT_MS[cmd] || config.timeoutMs;
      var timer = window.setTimeout(function () {
        if (!pending[id]) { return; }
        counters.timeouts++;
        settle(id, false, makeError(cmd, { code: 'timeout', message: cmd + ' did not answer in ' + ms + ' ms', hint: '' }));
      }, ms);
      pending[id] = { resolve: resolve, reject: reject, cmd: cmd, timer: timer };
      pendingCount++;
      try {
        transport.send(JSON.stringify({ id: id, cmd: cmd, params: params || {} }));
        counters.sent++;
      } catch (e) {
        settle(id, false, makeError(cmd, { code: 'internal', message: String(e && e.message), hint: '' }));
      }
    });
  }

  function on(name, fn) {
    if (typeof fn !== 'function') { throw new Error('VP_Bridge.on: handler must be a function'); }
    if (!handlers[name]) { handlers[name] = []; }
    handlers[name].push(fn);
    return function () { off(name, fn); };
  }

  function off(name, fn) {
    var list = handlers[name];
    if (!list) { return false; }
    var i = list.indexOf(fn);
    if (i < 0) { return false; }
    list.splice(i, 1);
    if (!list.length) { delete handlers[name]; }
    return true;
  }

  function handlerCount() {
    var n = 0;
    var names = Object.keys(handlers);
    for (var i = 0; i < names.length; i++) { n += handlers[names[i]].length; }
    return n;
  }

  function disconnect() {
    failAll({ code: 'no_engine', message: 'bridge disconnected', hint: '' });
    if (webviewHandle !== null && window.VP_Dom) { window.VP_Dom.off(webviewHandle); }
    webviewHandle = null;
    if (kindName === 'mock' && window.VP_MockEngine) { window.VP_MockEngine.disconnect(); }
    if (flushTimer !== null) { window.clearTimeout(flushTimer); }
    cancelFrame(flushFrame);
    flushTimer = null;
    flushFrame = null;
    flushScheduled = false;
    queue = [];
    transport = null;
    kindName = 'none';
  }

  function init(opts) {
    opts = opts || {};
    disconnect();
    config.timeoutMs = opts.timeoutMs || DEFAULT_TIMEOUT_MS;
    config.minFlushMs = opts.minFlushMs === undefined ? 100 : opts.minFlushMs;
    lastFlush = -1e9;
    var wv = window.chrome && window.chrome.webview;
    if (wv && opts.mock !== 'force') {
      webviewHandle = window.VP_Dom.on(wv, 'message', function (e) { receive(e.data); }, { owner: 'bridge' });
      transport = { send: function (text) { wv.postMessage(text); } };
      kindName = 'webview';
    } else if (opts.mock && window.VP_MockEngine) {
      window.VP_MockEngine.connect(receive);
      transport = { send: function (text) { window.VP_MockEngine.postMessage(text); } };
      kindName = 'mock';
    }
    return kindName;
  }

  function stats() {
    return {
      kind: kindName,
      pending: pendingCount,
      queued: queue.length,
      handlers: handlerCount(),
      sent: counters.sent,
      received: counters.received,
      events: counters.events,
      flushes: counters.flushes,
      delivered: counters.delivered,
      timeouts: counters.timeouts
    };
  }

  window.VP_Bridge = {
    init: init,
    disconnect: disconnect,
    call: call,
    on: on,
    off: off,
    handlerCount: handlerCount,
    stats: stats,
    kind: function () { return kindName; },
    isNative: function () { return kindName === 'webview'; },
    inject: receive
  };
}());
