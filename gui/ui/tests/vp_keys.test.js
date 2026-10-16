describe('VP_Keys', function () {
  function setup() {
    var env = load(['vp_dom.js', 'vp_i18n.js', 'vp_keys.js']);
    env.window.VP_I18n.load('en-US', { 'key.name.ctrl': 'Ctrl', 'key.name.shift': 'Shift', 'key.name.enter': 'Enter', 'key.name.arrowDown': '\u2193' });
    env.window.VP_I18n.load('es-MX', { 'key.name.ctrl': 'Ctrl', 'key.name.shift': 'Mayús', 'key.name.enter': 'Enter', 'key.name.arrowDown': '\u2193' });
    return env;
  }

  it('turns key events into combos', function () {
    var K = setup().window.VP_Keys;
    eq(K.comboOf({ key: 'j' }), 'J');
    eq(K.comboOf({ key: 'J', shiftKey: true }), 'Shift+J');
    eq(K.comboOf({ key: 'e', ctrlKey: true, shiftKey: true }), 'Ctrl+Shift+E');
    eq(K.comboOf({ key: '?', shiftKey: true }), '?');
    eq(K.comboOf({ key: ',', ctrlKey: true }), 'Ctrl+,');
    eq(K.comboOf({ key: ' ' }), 'Space');
    eq(K.comboOf({ key: 'Esc' }), 'Escape');
    eq(K.comboOf({ key: 'Enter', shiftKey: true }), 'Shift+Enter');
    eq(K.comboOf({ key: 'ArrowDown', metaKey: true }), 'Ctrl+ArrowDown');
    eq(K.comboOf({ key: 'Shift', shiftKey: true }), null);
  });

  it('dispatches to the newest handler and prevents the default', function () {
    var env = setup();
    var K = env.window.VP_Keys;
    K.bind(env.document);
    var log = [];
    K.handle('nextCue', function () { log.push('a'); });
    var off = K.handle('nextCue', function () { log.push('b'); });
    var allowed = env.key(env.document.body, 'j');
    deepEq(log, ['b']);
    eq(allowed, false, 'default prevented');
    off();
    env.key(env.document.body, 'ArrowDown');
    deepEq(log, ['b', 'a']);
    K.handle('save', function () { return false; });
    eq(env.key(env.document.body, 's', { ctrl: true }), true, 'handler returning false lets the key through');
    eq(env.key(env.document.body, 'q'), true, 'unbound key untouched');
  });

  it('ignores plain keys while typing, but not Ctrl combos, Escape or F1', function () {
    var env = setup();
    var K = env.window.VP_Keys;
    var D = env.window.VP_Dom;
    K.bind(env.document);
    var log = [];
    ['nextCue', 'edit', 'save', 'cancelEdit', 'help', 'accept'].forEach(function (a) { K.handle(a, function () { log.push(a); }); });
    var input = D.el('input', { type: 'text' });
    var area = D.el('textarea');
    var box = D.el('input', { type: 'checkbox' });
    var ce = D.el('div', { contenteditable: 'true' });
    env.document.body.appendChild(D.el('div', null, [input, area, box, ce]));
    env.key(input, 'j');
    env.key(area, 'e');
    env.key(area, 'Enter');
    env.key(ce, 'e');
    deepEq(log, []);
    env.key(input, 's', { ctrl: true });
    env.key(area, 'Escape');
    env.key(input, 'F1');
    env.key(box, 'j');
    deepEq(log, ['save', 'cancelEdit', 'help', 'nextCue']);
  });

  it('suspend() pauses global shortcuts (dialogs, tour); unbind removes the listener', function () {
    var env = setup();
    var K = env.window.VP_Keys;
    var h = K.bind(env.document);
    var n = 0;
    K.handle('help', function () { n++; });
    K.suspend();
    env.key(env.document.body, 'F1');
    eq(n, 0);
    K.resume();
    env.key(env.document.body, '?', { shift: true });
    eq(n, 1);
    eq(env.window.VP_Dom.count(), 1);
    K.unbind(h);
    eq(env.window.VP_Dom.count(), 0);
    throws(function () { K.handle('nope', function () {}); }, /unknown action/);
  });

  it('list() holds every shortcut of PREDESIGN 4.3; display() localises key names', function () {
    var env = setup();
    var K = env.window.VP_Keys;
    var combos = [];
    K.list().forEach(function (r) {
      ok(/^key\.\w+\.label$/.test(r.labelKey));
      combos = combos.concat(r.keys);
    });
    ['Ctrl+O', 'Ctrl+N', 'Ctrl+S', 'Ctrl+Z', 'Ctrl+Y', 'Ctrl+Shift+Z', 'ArrowDown', 'ArrowUp', 'J', 'K', 'Ctrl+ArrowDown', 'Ctrl+ArrowUp',
      'Enter', 'Shift+Enter', 'E', 'Escape', 'Ctrl+Enter', '1', '2', '3', 'Space', 'W', 'Ctrl+F', 'Ctrl+M', 'Ctrl+E', 'Ctrl+Shift+E', 'F1', '?', 'Ctrl+,'
    ].forEach(function (c) { ok(combos.indexOf(c) >= 0, 'missing ' + c); });
    eq(K.display('Ctrl+Shift+Enter'), 'Ctrl+Shift+Enter');
    eq(K.display('Ctrl+ArrowDown'), 'Ctrl+\u2193');
    env.window.VP_I18n.setLang('es-MX');
    eq(K.display('Shift+Enter'), 'Mayús+Enter');
    eq(K.display('Ctrl+,'), 'Ctrl+,');
    ok(K.i18nKeys().indexOf('key.group.file.title') >= 0);
  });
});
