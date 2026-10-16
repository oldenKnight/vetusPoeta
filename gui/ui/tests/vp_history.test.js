describe('VP_History', function () {
  function setup() { return load(['vp_history.js']); }

  it('push / undo / redo, and a new push clears redo', function () {
    var H = setup().window.VP_History;
    eq(H.canUndo(), false);
    H.push({ kind: 'edit', labelKey: 'history.edit.label', indices: [3], before: { target: 'a' }, after: { target: 'b' } });
    H.push({ kind: 'accept', indices: [4] });
    eq(H.size(), 2);
    eq(H.peekUndo().kind, 'accept');
    eq(H.undo().kind, 'accept');
    eq(H.canRedo(), true);
    eq(H.redo().kind, 'accept');
    H.undo();
    H.push({ kind: 'choose', indices: [5] });
    eq(H.redoSize(), 0);
    eq(H.redo(), null);
    throws(function () { H.push({}); }, /kind/);
  });

  it('keeps at most 500 entries and tells onTrimmed once', function () {
    var H = setup().window.VP_History;
    var told = [];
    H.onTrimmed(function (n) { told.push(n); });
    for (var i = 0; i < 600; i++) { H.push({ kind: 'edit', indices: [i] }); }
    eq(H.size(), 500);
    eq(H.peekUndo().indices[0], 599);
    eq(told.length, 1);
    H.clear();
    H.push({ kind: 'edit' });
    eq(H.size(), 1);
  });

  it('keeps at most 5 MB of diffs', function () {
    var H = setup().window.VP_History;
    var big = new Array(600001).join('x');
    for (var i = 0; i < 8; i++) { H.push({ kind: 'bulk', indices: [i], before: big, after: big }); }
    ok(H.bytes() <= H.limits().bytes, 'bytes ' + H.bytes());
    ok(H.size() < 8 && H.size() >= 1, 'size ' + H.size());
    eq(H.peekUndo().indices[0], 7);
  });

  it('mirrors the engine flags and counts its listeners', function () {
    var H = setup().window.VP_History;
    var changes = 0;
    var off = H.onChange(function () { changes++; });
    H.syncFromEngine({ canUndo: true, canRedo: false });
    ok(H.canUndo());
    deepEq(H.engineState(), { canUndo: true, canRedo: false });
    eq(H.listenerCount(), 1);
    off();
    eq(H.listenerCount(), 0);
    eq(changes, 1);
  });
});
