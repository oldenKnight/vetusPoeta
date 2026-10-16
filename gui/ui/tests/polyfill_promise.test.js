/* Promises/A+ basics of polyfill_promise.js, run where the native Promise was deleted. */
describe('polyfill_promise', function () {
  function setup() {
    var env = load([]);
    return { env: env, P: env.window.Promise, flush: env.clock.flush };
  }

  it('installs itself because the context has no Promise', function () {
    var s = setup();
    eq(typeof s.P, 'function');
    eq(s.P._vpPolyfill, true);
  });

  it('keeps a native Promise when one exists', function () {
    var env = load(['polyfill_promise.js'], { noPolyfill: true, keepPromise: true });
    eq(env.run('typeof Promise._vpPolyfill'), 'undefined');
  });

  it('calls then() callbacks asynchronously, never in the same turn', function () {
    var s = setup();
    var log = [];
    s.P.resolve(1).then(function (v) { log.push('then ' + v); });
    log.push('sync');
    deepEq(log, ['sync']);
    s.flush();
    deepEq(log, ['sync', 'then 1']);
  });

  it('runs callbacks in registration order, including ones added while draining', function () {
    var s = setup();
    var log = [];
    var p = s.P.resolve('x');
    p.then(function () {
      log.push('a');
      p.then(function () { log.push('c'); });
    });
    p.then(function () { log.push('b'); });
    s.flush();
    deepEq(log, ['a', 'b', 'c']);
  });

  it('chains values, adopts returned promises and thenables', function () {
    var s = setup();
    var out = null;
    s.P.resolve(1).then(function (v) { return v + 1; }).then(function (v) {
      return new s.P(function (resolve) { resolve(v * 10); });
    }).then(function (v) {
      return { then: function (ok) { ok(v + 5); } };
    }).then(function (v) { out = v; });
    s.flush();
    eq(out, 25);
  });

  it('turns a thrown error into a rejection that catch() receives', function () {
    var s = setup();
    var caught = null;
    s.P.resolve().then(function () { throw new Error('boom'); }).then(function () { caught = 'wrong'; })['catch'](function (e) { caught = e.message; });
    s.flush();
    eq(caught, 'boom');
  });

  it('passes values and reasons through missing handlers', function () {
    var s = setup();
    var got = [];
    s.P.resolve(7).then(null).then(function (v) { got.push(v); });
    s.P.reject('r').then(function () { got.push('no'); }).then(null, function (r) { got.push(r); });
    s.flush();
    deepEq(got, [7, 'r']);
  });

  it('settles once: later resolve/reject calls are ignored', function () {
    var s = setup();
    var got = [];
    var p = new s.P(function (resolve, reject) {
      resolve(1);
      resolve(2);
      reject(3);
      throw new Error('after resolve');
    });
    p.then(function (v) { got.push(v); }, function () { got.push('rejected'); });
    s.flush();
    deepEq(got, [1]);
  });

  it('rejects when the executor throws', function () {
    var s = setup();
    var reason = null;
    new s.P(function () { throw new Error('exec'); }).then(null, function (e) { reason = e.message; });
    s.flush();
    eq(reason, 'exec');
  });

  it('rejects a promise resolved with itself with a TypeError', function () {
    var s = setup();
    var err = null;
    var p = s.P.resolve().then(function () { return p; });
    p.then(null, function (e) { err = e; });
    s.flush();
    ok(err instanceof s.env.run('TypeError'), 'a TypeError of the page realm');
    ok(/itself/.test(err.message));
  });

  it('uses only the first call of a misbehaving thenable', function () {
    var s = setup();
    var got = [];
    s.P.resolve({ then: function (ok, no) { ok('first'); no('second'); ok('third'); } }).then(function (v) { got.push(v); }, function (r) { got.push('rej ' + r); });
    s.flush();
    deepEq(got, ['first']);
  });

  it('all() keeps input order and rejects on the first rejection', function () {
    var s = setup();
    var env = s.env;
    var res = null;
    var slow = new s.P(function (resolve) { env.window.setTimeout(function () { resolve('slow'); }, 50); });
    s.P.all([slow, 2, s.P.resolve(3)]).then(function (v) { res = v; });
    var rej = null;
    s.P.all([slow, s.P.reject('bad')]).then(null, function (r) { rej = r; });
    var empty = null;
    s.P.all([]).then(function (v) { empty = v; });
    env.clock.tick(60);
    deepEq(res, ['slow', 2, 3]);
    eq(rej, 'bad');
    deepEq(empty, []);
  });

  it('race() settles with the first one', function () {
    var s = setup();
    var env = s.env;
    var res = null;
    var a = new s.P(function (resolve) { env.window.setTimeout(function () { resolve('a'); }, 30); });
    var b = new s.P(function (resolve) { env.window.setTimeout(function () { resolve('b'); }, 10); });
    s.P.race([a, b]).then(function (v) { res = v; });
    env.clock.tick(40);
    eq(res, 'b');
  });

  it('resolve() returns the same promise for a polyfill promise', function () {
    var s = setup();
    var p = s.P.resolve(1);
    eq(s.P.resolve(p), p);
    throws(function () { s.P(function () {}); }, /new/);
    throws(function () { return new s.P(5); }, /function/);
  });
});
