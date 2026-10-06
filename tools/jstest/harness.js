/* harness.js - the unit-test harness (PREPLAN 4.4 item 4): describe, it, eq, deepEq, ok, throws.
 * A test whose function takes an argument is asynchronous and must call done() (or done(err)).
 */
'use strict';

function AssertionError(msg) { this.message = msg; this.name = 'AssertionError'; this.stack = (new Error(msg)).stack; }
AssertionError.prototype = Object.create(Error.prototype);

function show(v) {
  try {
    var s = JSON.stringify(v);
    if (s === undefined) { return String(v); }
    return s.length > 200 ? s.slice(0, 200) + '...' : s;
  } catch (e) { return String(v); }
}

function kind(v) { return Object.prototype.toString.call(v); }

function deepEqual(a, b, seen) {
  if (a === b) { return true; }
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) {
    return typeof a === 'number' && typeof b === 'number' && a !== a && b !== b;
  }
  if (kind(a) !== kind(b)) { return false; }
  seen = seen || [];
  for (var s = 0; s < seen.length; s++) { if (seen[s][0] === a && seen[s][1] === b) { return true; } }
  seen.push([a, b]);
  var ka = Object.keys(a);
  var kb = Object.keys(b);
  if (ka.length !== kb.length) { return false; }
  for (var i = 0; i < ka.length; i++) {
    if (!Object.prototype.hasOwnProperty.call(b, ka[i])) { return false; }
    if (!deepEqual(a[ka[i]], b[ka[i]], seen)) { return false; }
  }
  return true;
}

var asserts = {
  eq: function (actual, expected, msg) {
    if (actual !== expected) { throw new AssertionError((msg ? msg + ': ' : '') + 'expected ' + show(expected) + ', got ' + show(actual)); }
  },
  deepEq: function (actual, expected, msg) {
    if (!deepEqual(actual, expected)) { throw new AssertionError((msg ? msg + ': ' : '') + 'expected ' + show(expected) + ', got ' + show(actual)); }
  },
  ok: function (value, msg) {
    if (!value) { throw new AssertionError(msg || ('expected a truthy value, got ' + show(value))); }
  },
  throws: function (fn, re, msg) {
    var threw = false;
    try { fn(); } catch (e) {
      threw = true;
      if (re && !re.test(String(e && e.message))) { throw new AssertionError((msg ? msg + ': ' : '') + 'error "' + (e && e.message) + '" does not match ' + re); }
    }
    if (!threw) { throw new AssertionError(msg || 'expected an exception'); }
  }
};

function Suite() {
  this.tests = [];
  this.stack = [];
  var self = this;
  this.describe = function (name, fn) {
    self.stack.push(name);
    try { fn(); } finally { self.stack.pop(); }
  };
  this.it = function (name, fn) {
    self.tests.push({ name: self.stack.concat([name]).join(' > '), fn: fn });
  };
}

// Runs the tests one by one; calls back with [{name, ok, error, ms}].
Suite.prototype.run = function (opts, cb) {
  var tests = this.tests;
  var results = [];
  var timeoutMs = (opts && opts.timeoutMs) || 3000;
  var i = 0;
  function next() {
    if (i >= tests.length) { cb(results); return; }
    var t = tests[i++];
    var started = Date.now();
    var finished = false;
    var timer = null;
    function finish(err) {
      if (finished) { return; }
      finished = true;
      if (timer) { clearTimeout(timer); }
      results.push({ name: t.name, ok: !err, error: err ? (err.stack && err.name !== 'AssertionError' ? String(err.stack).split('\n').slice(0, 4).join('\n      ') : String(err.message || err)) : null, ms: Date.now() - started });
      setImmediate(next);
    }
    try {
      if (t.fn.length >= 1) {
        timer = setTimeout(function () { finish(new Error('timeout after ' + timeoutMs + ' ms (done() not called)')); }, timeoutMs);
        t.fn(function (err) { finish(err || null); });
      } else {
        t.fn();
        finish(null);
      }
    } catch (e) {
      finish(e);
    }
  }
  next();
};

module.exports = { Suite: Suite, asserts: asserts, deepEqual: deepEqual, AssertionError: AssertionError };
