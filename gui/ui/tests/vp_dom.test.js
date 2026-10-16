describe('VP_Dom', function () {
  function setup() { return load(['vp_dom.js']); }

  it('el() builds elements with attributes, properties, dataset, style and children', function () {
    var env = setup();
    var D = env.window.VP_Dom;
    var child = D.el('span', { text: 'inner' });
    var node = D.el('button', {
      className: 'vp-btn a', type: 'button', 'aria-label': 'Close', disabled: true, hidden: false,
      dataset: { cueIndex: 4 }, style: { width: '40%' }, 'data-i18n-vars': { n: 2 }, title: null
    }, ['text ', child, null, [D.el('b', null, 'x')], 7]);
    eq(node.localName, 'button');
    eq(node.className, 'vp-btn a');
    eq(node.getAttribute('aria-label'), 'Close');
    eq(node.disabled, true);
    eq(node.hasAttribute('hidden'), false);
    eq(node.getAttribute('data-cue-index'), '4');
    eq(node.style.width, '40%');
    eq(node.getAttribute('data-i18n-vars'), '{"n":2}');
    eq(node.hasAttribute('title'), false);
    eq(node.textContent, 'text innerx7');
    eq(env.innerHTMLWrites, 0);
  });

  it('el() creates SVG in the SVG namespace', function () {
    var env = setup();
    var svg = env.window.VP_Dom.el('svg', { className: 'vp-icon' }, [env.window.VP_Dom.el('use', { href: '#vp-i-close' })]);
    eq(svg.namespaceURI, 'http://www.w3.org/2000/svg');
    eq(svg.getAttribute('class'), 'vp-icon');
    eq(svg.firstChild.getAttribute('href'), '#vp-i-close');
  });

  it('on/off keep an exact registry and the real listener count', function () {
    var env = setup();
    var D = env.window.VP_Dom;
    var b = D.el('button');
    env.document.body.appendChild(b);
    var hits = 0;
    function f() { hits++; }
    var h1 = D.on(b, 'click', f);
    D.on(b, 'keydown', f, { owner: 'screen' });
    D.on(env.document, 'click', f, { owner: 'screen', capture: true });
    eq(D.count(), 3);
    eq(D.count('screen'), 2);
    eq(env.listenerCount(), 3);
    b.click();
    eq(hits, 2);
    ok(D.off(h1));
    eq(D.off(h1), false);
    eq(D.offOwner('screen'), 2);
    eq(D.count(), 0);
    eq(env.listenerCount(), 0);
    D.on(b, 'click', f);
    ok(D.off(b, 'click', f));
    eq(env.listenerCount(), 0);
    throws(function () { D.on(null, 'click', f); }, /cannot listen/);
  });

  it('delegate() fires for matching descendants only, with the matched element', function () {
    var env = setup();
    var D = env.window.VP_Dom;
    var inner = D.el('span', { text: 'x' });
    var btn = D.el('button', { className: 'go' }, [inner]);
    var other = D.el('p');
    var root = D.el('div', null, [btn, other]);
    env.document.body.appendChild(root);
    var seen = [];
    D.delegate(root, 'button.go', 'click', function (e, el) { seen.push(el === btn && this === btn); });
    inner.click();
    other.click();
    deepEq(seen, [true]);
    eq(D.count(), 1);
  });

  it('clear() empties a node; focusables() skips disabled, hidden and tabindex -1', function () {
    var env = setup();
    var D = env.window.VP_Dom;
    var root = D.el('div', null, [
      D.el('button', { id: 'a' }), D.el('button', { id: 'b', disabled: true }),
      D.el('div', { hidden: true }, [D.el('input', { id: 'c' })]),
      D.el('a', { id: 'd' }), D.el('a', { id: 'e', href: '#x' }),
      D.el('div', { id: 'f', tabIndex: 0 }), D.el('div', { id: 'g', tabIndex: -1 }), D.el('textarea', { id: 'h' })
    ]);
    deepEq(D.focusables(root).map(function (n) { return n.id; }), ['a', 'e', 'f', 'h']);
    D.clear(root);
    eq(root.childNodes.length, 0);
  });
});
