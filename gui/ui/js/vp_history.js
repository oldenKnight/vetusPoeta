/* vp_history.js - the UI's mirror of the undo history (PREDESIGN 4.4). The engine is
 * authoritative (history.undo / history.redo); this mirror gives instant labels and the
 * local before/after of each command. Cap: 500 entries or 5 MB, whichever comes first; the
 * oldest entries are dropped and onTrimmed listeners hear about it once per project.
 *
 * VP_History.push({kind, labelKey, indices, before, after}) -> entry
 * VP_History.undo() / redo() -> entry or null; canUndo(), canRedo(), peekUndo(), peekRedo()
 * VP_History.syncFromEngine({canUndo, canRedo}); engineState()
 * VP_History.clear() (project close), size(), bytes(), onChange(fn) / onTrimmed(fn) -> remover
 */
(function () {
  'use strict';

  var MAX_ENTRIES = 500;
  var MAX_BYTES = 5 * 1024 * 1024;

  var undoStack = [];
  var redoStack = [];
  var totalBytes = 0;
  var trimmedOnce = false;
  var engine = { canUndo: false, canRedo: false };
  var changeFns = [];
  var trimFns = [];

  function sizeOf(entry) {
    var text;
    try {
      text = JSON.stringify([entry.indices, entry.before, entry.after]) || '';
    } catch (e) {
      text = '';
    }
    return text.length * 2 + 64;
  }

  function call(list, arg) {
    var copy = list.slice();
    for (var i = 0; i < copy.length; i++) {
      try {
        copy[i](arg);
      } catch (e) {
        if (window.console) { window.console.error('[VP_History] listener failed', e); }
      }
    }
  }

  function trim() {
    var dropped = 0;
    while (undoStack.length > 1 && (undoStack.length > MAX_ENTRIES || totalBytes > MAX_BYTES)) {
      totalBytes -= undoStack.shift().size;
      dropped++;
    }
    if (dropped && !trimmedOnce) {
      trimmedOnce = true;
      call(trimFns, dropped);
    }
    return dropped;
  }

  function push(entry) {
    if (!entry || typeof entry.kind !== 'string') { throw new Error('VP_History.push: entry needs a kind'); }
    var e = {
      kind: entry.kind,
      labelKey: entry.labelKey || '',
      indices: entry.indices || [],
      before: entry.before === undefined ? null : entry.before,
      after: entry.after === undefined ? null : entry.after
    };
    e.size = sizeOf(e);
    for (var i = 0; i < redoStack.length; i++) { totalBytes -= redoStack[i].size; }
    redoStack = [];
    undoStack.push(e);
    totalBytes += e.size;
    trim();
    call(changeFns, 'push');
    return e;
  }

  function undo() {
    var e = undoStack.pop();
    if (!e) { return null; }
    redoStack.push(e);
    call(changeFns, 'undo');
    return e;
  }

  function redo() {
    var e = redoStack.pop();
    if (!e) { return null; }
    undoStack.push(e);
    call(changeFns, 'redo');
    return e;
  }

  function clear() {
    undoStack = [];
    redoStack = [];
    totalBytes = 0;
    trimmedOnce = false;
    engine = { canUndo: false, canRedo: false };
    call(changeFns, 'clear');
  }

  function listen(list, fn) {
    if (typeof fn !== 'function') { throw new Error('VP_History: listener must be a function'); }
    list.push(fn);
    return function () {
      var i = list.indexOf(fn);
      if (i >= 0) { list.splice(i, 1); }
    };
  }

  window.VP_History = {
    push: push,
    undo: undo,
    redo: redo,
    clear: clear,
    canUndo: function () { return undoStack.length > 0 || engine.canUndo; },
    canRedo: function () { return redoStack.length > 0 || engine.canRedo; },
    peekUndo: function () { return undoStack.length ? undoStack[undoStack.length - 1] : null; },
    peekRedo: function () { return redoStack.length ? redoStack[redoStack.length - 1] : null; },
    syncFromEngine: function (r) {
      engine = { canUndo: !!(r && r.canUndo), canRedo: !!(r && r.canRedo) };
      call(changeFns, 'sync');
    },
    engineState: function () { return { canUndo: engine.canUndo, canRedo: engine.canRedo }; },
    size: function () { return undoStack.length; },
    redoSize: function () { return redoStack.length; },
    bytes: function () { return totalBytes; },
    limits: function () { return { entries: MAX_ENTRIES, bytes: MAX_BYTES }; },
    onChange: function (fn) { return listen(changeFns, fn); },
    onTrimmed: function (fn) { return listen(trimFns, fn); },
    listenerCount: function () { return changeFns.length + trimFns.length; }
  };
}());
