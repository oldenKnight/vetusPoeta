/* polyfill_promise.js - a small Promises/A+ implementation for engines without Promise.
 *
 * Defines window.Promise only when it is missing (WebView2 always has a native one; the
 * jstest runner deletes it to prove the UI never depends on more than this file).
 * Supports: new Promise(executor), then, catch, Promise.resolve, reject, all, race.
 * Callbacks run asynchronously, in registration order, from one queue drained per task.
 */
(function () {
  'use strict';

  if (typeof window.Promise === 'function') { return; }

  var PENDING = 0;
  var FULFILLED = 1;
  var REJECTED = 2;

  var queue = [];
  var draining = false;

  function drain() {
    var i = 0;
    while (i < queue.length) {
      var job = queue[i];
      queue[i] = null;
      i++;
      job();
    }
    queue = [];
    draining = false;
  }

  function asap(fn) {
    queue.push(fn);
    if (!draining) {
      draining = true;
      window.setTimeout(drain, 0);
    }
  }

  function noop() {}

  function settle(p, state, value) {
    if (p._state !== PENDING) { return; }
    p._state = state;
    p._value = value;
    var handlers = p._handlers;
    p._handlers = null;
    for (var i = 0; i < handlers.length; i++) { schedule(p, handlers[i]); }
  }

  function schedule(p, h) {
    asap(function () { runHandler(p, h); });
  }

  function runHandler(p, h) {
    var cb = p._state === FULFILLED ? h.onFulfilled : h.onRejected;
    if (cb === null) {
      if (p._state === FULFILLED) { resolveWith(h.next, p._value); } else { settle(h.next, REJECTED, p._value); }
      return;
    }
    var result;
    try {
      result = cb(p._value);
    } catch (e) {
      settle(h.next, REJECTED, e);
      return;
    }
    resolveWith(h.next, result);
  }

  // The Promise Resolution Procedure (A+ 2.3).
  function resolveWith(p, x) {
    if (x === p) {
      settle(p, REJECTED, new TypeError('A promise cannot be resolved with itself'));
      return;
    }
    if (x !== null && (typeof x === 'object' || typeof x === 'function')) {
      var then;
      try {
        then = x.then;
      } catch (e) {
        settle(p, REJECTED, e);
        return;
      }
      if (typeof then === 'function') {
        var called = false;
        try {
          then.call(x, function (y) {
            if (called) { return; }
            called = true;
            resolveWith(p, y);
          }, function (r) {
            if (called) { return; }
            called = true;
            settle(p, REJECTED, r);
          });
        } catch (e2) {
          if (!called) {
            called = true;
            settle(p, REJECTED, e2);
          }
        }
        return;
      }
    }
    settle(p, FULFILLED, x);
  }

  function P(executor) {
    if (!(this instanceof P)) { throw new TypeError('Promise must be called with new'); }
    if (typeof executor !== 'function') { throw new TypeError('Promise executor must be a function'); }
    this._state = PENDING;
    this._value = undefined;
    this._handlers = [];
    var self = this;
    var done = false;
    try {
      executor(function (value) {
        if (done) { return; }
        done = true;
        resolveWith(self, value);
      }, function (reason) {
        if (done) { return; }
        done = true;
        settle(self, REJECTED, reason);
      });
    } catch (e) {
      if (!done) {
        done = true;
        settle(self, REJECTED, e);
      }
    }
  }

  P.prototype.then = function (onFulfilled, onRejected) {
    var next = new P(noop);
    var h = {
      onFulfilled: typeof onFulfilled === 'function' ? onFulfilled : null,
      onRejected: typeof onRejected === 'function' ? onRejected : null,
      next: next
    };
    if (this._state === PENDING) { this._handlers.push(h); } else { schedule(this, h); }
    return next;
  };

  P.prototype['catch'] = function (onRejected) {
    return this.then(null, onRejected);
  };

  P.resolve = function (value) {
    if (value instanceof P) { return value; }
    return new P(function (resolve) { resolve(value); });
  };

  P.reject = function (reason) {
    return new P(function (resolve, reject) { reject(reason); });
  };

  P.all = function (list) {
    return new P(function (resolve, reject) {
      var items = Array.prototype.slice.call(list || []);
      var results = new Array(items.length);
      var remaining = items.length;
      if (remaining === 0) {
        resolve(results);
        return;
      }
      function settleAt(i) {
        P.resolve(items[i]).then(function (v) {
          results[i] = v;
          remaining--;
          if (remaining === 0) { resolve(results); }
        }, reject);
      }
      for (var i = 0; i < items.length; i++) { settleAt(i); }
    });
  };

  P.race = function (list) {
    return new P(function (resolve, reject) {
      var items = Array.prototype.slice.call(list || []);
      for (var i = 0; i < items.length; i++) { P.resolve(items[i]).then(resolve, reject); }
    });
  };

  P._vpPolyfill = true;

  window.Promise = P;
}());
