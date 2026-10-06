/* vp_keys.js - the one keyboard shortcut table (PREDESIGN 4.3) and its dispatcher.
 *
 * VP_Keys.bind(root) -> handle (one keydown listener through VP_Dom); unbind(handle)
 * VP_Keys.handle(action, fn, owner) -> remover; the newest handler of an action wins;
 *   fn(event) returning false lets the key through (no preventDefault)
 * VP_Keys.list() -> [{id, keys:[combo], group, labelKey}] for the help popover
 * VP_Keys.display(combo) -> localised key names ("Ctrl+Shift+E", "Mayús+Intro" ...)
 * VP_Keys.suspend() / resume(): dialogs and the tour stop global shortcuts while open
 * Keys are ignored while typing in an input, textarea, select or contenteditable unless they
 * use Ctrl (Escape and F1 still work there). Combos are written Ctrl+Shift+Alt+Key; letters
 * upper case; Shift is implied for symbols such as "?".
 */
(function () {
  'use strict';

  var TABLE = [
    { id: 'open', keys: ['Ctrl+O'], group: 'file' },
    { id: 'new', keys: ['Ctrl+N'], group: 'file' },
    { id: 'save', keys: ['Ctrl+S'], group: 'file' },
    { id: 'export', keys: ['Ctrl+Shift+E'], group: 'file' },
    { id: 'undo', keys: ['Ctrl+Z'], group: 'edit' },
    { id: 'redo', keys: ['Ctrl+Y', 'Ctrl+Shift+Z'], group: 'edit' },
    { id: 'nextCue', keys: ['ArrowDown', 'J'], group: 'navigate' },
    { id: 'prevCue', keys: ['ArrowUp', 'K'], group: 'navigate' },
    { id: 'nextReview', keys: ['Ctrl+ArrowDown'], group: 'navigate' },
    { id: 'prevReview', keys: ['Ctrl+ArrowUp'], group: 'navigate' },
    { id: 'search', keys: ['Ctrl+F'], group: 'navigate' },
    { id: 'accept', keys: ['Enter'], group: 'review' },
    { id: 'acceptNext', keys: ['Shift+Enter'], group: 'review' },
    { id: 'edit', keys: ['E'], group: 'review' },
    { id: 'cancelEdit', keys: ['Escape'], group: 'review' },
    { id: 'acceptEdit', keys: ['Ctrl+Enter'], group: 'review' },
    { id: 'alt1', keys: ['1'], group: 'review' },
    { id: 'alt2', keys: ['2'], group: 'review' },
    { id: 'alt3', keys: ['3'], group: 'review' },
    { id: 'inspectWord', keys: ['Space'], group: 'review' },
    { id: 'why', keys: ['W'], group: 'review' },
    { id: 'macrons', keys: ['Ctrl+M'], group: 'view' },
    { id: 'emoji', keys: ['Ctrl+E'], group: 'view' },
    { id: 'help', keys: ['F1', '?'], group: 'help' },
    { id: 'settings', keys: ['Ctrl+,'], group: 'help' }
  ];
  var GROUPS = ['file', 'edit', 'navigate', 'review', 'view', 'help'];
  var NAMED = { Ctrl: 'ctrl', Shift: 'shift', Alt: 'alt', Enter: 'enter', Escape: 'escape', Space: 'space', ArrowDown: 'arrowDown', ArrowUp: 'arrowUp' };
  var TYPING_OK = { Escape: true, F1: true };
  var ALIASES = { ' ': 'Space', Spacebar: 'Space', Esc: 'Escape', Down: 'ArrowDown', Up: 'ArrowUp', Left: 'ArrowLeft', Right: 'ArrowRight' };
  var MODIFIERS = { Control: 1, Shift: 1, Alt: 1, Meta: 1, OS: 1, AltGraph: 1 };

  var byCombo = {};
  var handlers = {};
  var suspended = 0;

  (function index() {
    for (var i = 0; i < TABLE.length; i++) {
      for (var k = 0; k < TABLE[i].keys.length; k++) {
        var combo = TABLE[i].keys[k];
        if (!byCombo[combo]) { byCombo[combo] = []; }
        byCombo[combo].push(TABLE[i].id);
      }
    }
  }());

  function comboOf(e) {
    var key = e.key;
    if (!key || MODIFIERS[key] === 1) { return null; }
    if (ALIASES[key]) { key = ALIASES[key]; }
    var named = key.length > 1;
    if (!named) { key = key.toUpperCase(); }
    var isLetter = !named && key.toLowerCase() !== key;
    var parts = [];
    if (e.ctrlKey || e.metaKey) { parts.push('Ctrl'); }
    if (e.shiftKey && (named || isLetter)) { parts.push('Shift'); }
    if (e.altKey) { parts.push('Alt'); }
    parts.push(key);
    return parts.join('+');
  }

  function isTyping(target) {
    if (!target || target.nodeType !== 1) { return false; }
    var tag = target.localName;
    if (tag === 'textarea' || tag === 'select') { return true; }
    if (tag === 'input') {
      var type = (target.getAttribute('type') || 'text').toLowerCase();
      return ['button', 'checkbox', 'radio', 'submit', 'reset', 'range', 'color', 'file'].indexOf(type) < 0;
    }
    var ce = target.getAttribute('contenteditable');
    return ce !== null && ce !== 'false';
  }

  function onKey(e) {
    if (suspended > 0) { return; }
    var combo = comboOf(e);
    if (!combo) { return; }
    var actions = byCombo[combo];
    if (!actions) { return; }
    if (isTyping(e.target) && !(e.ctrlKey || e.metaKey) && TYPING_OK[combo] !== true) { return; }
    for (var i = 0; i < actions.length; i++) {
      var list = handlers[actions[i]];
      if (!list || !list.length) { continue; }
      var result = list[list.length - 1].fn(e, actions[i]);
      if (result !== false) { e.preventDefault(); }
      return;
    }
  }

  function bind(root) {
    return window.VP_Dom.on(root || document, 'keydown', onKey, { owner: 'keys' });
  }

  function unbind(handle) {
    return window.VP_Dom.off(handle);
  }

  function known(action) {
    for (var i = 0; i < TABLE.length; i++) { if (TABLE[i].id === action) { return true; } }
    return false;
  }

  function handle(action, fn, owner) {
    if (!known(action)) { throw new Error('VP_Keys.handle: unknown action "' + action + '"'); }
    if (typeof fn !== 'function') { throw new Error('VP_Keys.handle: not a function'); }
    if (!handlers[action]) { handlers[action] = []; }
    var entry = { fn: fn, owner: owner || '' };
    handlers[action].push(entry);
    return function () {
      var list = handlers[action];
      var i = list ? list.indexOf(entry) : -1;
      if (i >= 0) { list.splice(i, 1); }
    };
  }

  function handlerCount() {
    var n = 0;
    var ids = Object.keys(handlers);
    for (var i = 0; i < ids.length; i++) { n += handlers[ids[i]].length; }
    return n;
  }

  function list() {
    return TABLE.map(function (row) {
      return { id: row.id, keys: row.keys.slice(), group: row.group, labelKey: 'key.' + row.id + '.label' };
    });
  }

  function display(combo) {
    var parts = combo === '+' ? ['+'] : combo.split('+');
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i] === '' ? '+' : parts[i];
      out.push(NAMED[p] ? window.VP_I18n.t('key.name.' + NAMED[p]) : p);
    }
    return out.join('+');
  }

  function i18nKeys() {
    var keys = [];
    var i;
    for (i = 0; i < TABLE.length; i++) { keys.push('key.' + TABLE[i].id + '.label'); }
    for (i = 0; i < GROUPS.length; i++) { keys.push('key.group.' + GROUPS[i] + '.title'); }
    var names = Object.keys(NAMED);
    for (i = 0; i < names.length; i++) { keys.push('key.name.' + NAMED[names[i]]); }
    return keys;
  }

  window.VP_Keys = {
    bind: bind,
    unbind: unbind,
    handle: handle,
    handlerCount: handlerCount,
    list: list,
    groups: function () { return GROUPS.slice(); },
    display: display,
    comboOf: comboOf,
    isTyping: isTyping,
    suspend: function () { suspended++; },
    resume: function () { suspended = Math.max(0, suspended - 1); },
    isSuspended: function () { return suspended > 0; },
    i18nKeys: i18nKeys
  };
}());
