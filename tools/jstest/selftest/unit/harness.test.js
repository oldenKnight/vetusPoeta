// expect-unit: 4 3
describe('harness self test', function () {
  it('eq passes', function () { eq(1 + 1, 2); });
  it('deepEq passes across objects', function () { deepEq({ a: [1, 2], b: { c: 'x' } }, { a: [1, 2], b: { c: 'x' } }); });
  it('throws passes', function () { throws(function () { throw new Error('boom'); }, /boom/); });
  it('async done passes', function (done) { setTimeout(function () { done(); }, 5); });
  it('eq mismatch fails', function () { eq('a', 'b'); });
  it('an exception fails', function () { var o = null; return o.x; });
  it('a missing done() times out', function (done) { return done && null; });
});
