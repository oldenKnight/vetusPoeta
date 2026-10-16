/* vp_dom.js - element builder and the one place listeners are added (DESIGN 13).
 *
 * VP_Dom.el(tag, attrs, children)   build an element; attrs: className, text, dataset{},
 *                                   style{}, aria-* / data-* / any attribute, and the
 *                                   properties value, checked, disabled, hidden, tabIndex, id
 * VP_Dom.on(target, type, fn, opts) -> handle; opts {capture, owner}; every listener of the UI
 *                                   goes through here so count() can prove nothing leaks
 * VP_Dom.off(handle) | off(target, type, fn), offOwner(owner), count(owner?)
 * VP_Dom.delegate(root, selector, type, fn, opts) one listener for many children;
 *                                   fn(event, matchedElement)
 * VP_Dom.clear(node), append(node, children), qs, qsa, closest, matches, focusables(root)
 */
(function () {
  'use strict';

  var SVG_NS = 'http://www.w3.org/2000/svg';
  var SVG_TAGS = { svg: 1, use: 1, path: 1, circle: 1, rect: 1, g: 1, polygon: 1, line: 1, symbol: 1, title: 1 };
  var PROPS = { value: 1, checked: 1, disabled: 1, hidden: 1, tabIndex: 1, id: 1, htmlFor: 1, selected: 1 };

  var registry = {};
  var live = 0;
  var nextId = 1;

  function on(target, type, handler, opts) {
    if (!target || typeof target.addEventListener !== 'function') { throw new Error('VP_Dom.on: target cannot listen to "' + type + '"'); }
    if (typeof handler !== 'function') { throw new Error('VP_Dom.on: handler must be a function'); }
    var capture = !!(opts && opts.capture);
    var id = nextId++;
    registry[id] = { target: target, type: type, handler: handler, capture: capture, owner: (opts && opts.owner) || '' };
    target.addEventListener(type, handler, capture);
    live++;
    return id;
  }

  function removeEntry(id) {
    var r = registry[id];
    if (!r) { return false; }
    r.target.removeEventListener(r.type, r.handler, r.capture);
    delete registry[id];
    live--;
    return true;
  }

  function off(handleOrTarget, type, handler) {
    if (typeof handleOrTarget === 'number') { return removeEntry(handleOrTarget); }
    var ids = Object.keys(registry);
    for (var i = 0; i < ids.length; i++) {
      var r = registry[ids[i]];
      if (r.target === handleOrTarget && r.type === type && r.handler === handler) { return removeEntry(Number(ids[i])); }
    }
    return false;
  }

  function offOwner(owner) {
    var n = 0;
    var ids = Object.keys(registry);
    for (var i = 0; i < ids.length; i++) {
      if (registry[ids[i]].owner === owner) {
        removeEntry(Number(ids[i]));
        n++;
      }
    }
    return n;
  }

  function count(owner) {
    if (owner === undefined) { return live; }
    var n = 0;
    var ids = Object.keys(registry);
    for (var i = 0; i < ids.length; i++) { if (registry[ids[i]].owner === owner) { n++; } }
    return n;
  }

  function matches(el, selector) {
    if (!el || el.nodeType !== 1) { return false; }
    var fn = el.matches || el.msMatchesSelector || el.webkitMatchesSelector;
    return fn ? fn.call(el, selector) : false;
  }

  function closest(node, selector, stopAt) {
    while (node && node.nodeType === 1) {
      if (matches(node, selector)) { return node; }
      if (node === stopAt) { return null; }
      node = node.parentNode;
    }
    return null;
  }

  function delegate(root, selector, type, handler, opts) {
    return on(root, type, function (e) {
      var hit = closest(e.target, selector, root);
      if (hit) { handler.call(hit, e, hit); }
    }, opts);
  }

  function append(parent, children) {
    if (children === null || children === undefined || children === false) { return parent; }
    if (Object.prototype.toString.call(children) !== '[object Array]') { children = [children]; }
    for (var i = 0; i < children.length; i++) {
      var c = children[i];
      if (c === null || c === undefined || c === false) { continue; }
      if (Object.prototype.toString.call(c) === '[object Array]') {
        append(parent, c);
      } else if (typeof c === 'string' || typeof c === 'number') {
        parent.appendChild(document.createTextNode(String(c)));
      } else {
        parent.appendChild(c);
      }
    }
    return parent;
  }

  function el(tag, attrs, children) {
    var node = SVG_TAGS[tag] === 1 ? document.createElementNS(SVG_NS, tag) : document.createElement(tag);
    if (attrs) {
      var keys = Object.keys(attrs);
      for (var i = 0; i < keys.length; i++) {
        var k = keys[i];
        var v = attrs[k];
        if (v === null || v === undefined) { continue; }
        if (k === 'className') {
          if (node.namespaceURI === SVG_NS) { node.setAttribute('class', v); } else { node.className = v; }
        } else if (k === 'text') {
          node.textContent = String(v);
        } else if (k === 'dataset') {
          var dk = Object.keys(v);
          for (var j = 0; j < dk.length; j++) { node.setAttribute('data-' + dk[j].replace(/[A-Z]/g, kebabChar), String(v[dk[j]])); }
        } else if (k === 'style') {
          var sk = Object.keys(v);
          for (var s = 0; s < sk.length; s++) { node.style[sk[s]] = v[sk[s]]; }
        } else if (k === 'data-i18n-vars' && typeof v === 'object') {
          node.setAttribute(k, JSON.stringify(v));
        } else if (PROPS[k] === 1) {
          node[k] = v;
        } else if (v === true) {
          node.setAttribute(k, '');
        } else if (v !== false) {
          node.setAttribute(k, String(v));
        }
      }
    }
    append(node, children);
    return node;
  }

  function kebabChar(c) { return '-' + c.toLowerCase(); }

  function clear(node) {
    if (!node) { return node; }
    while (node.firstChild) { node.removeChild(node.firstChild); }
    return node;
  }

  function qs(selector, root) { return (root || document).querySelector(selector); }

  function qsa(selector, root) {
    return Array.prototype.slice.call((root || document).querySelectorAll(selector));
  }

  function isHidden(node, root) {
    while (node && node !== root) {
      if (node.nodeType === 1 && node.hasAttribute('hidden')) { return true; }
      node = node.parentNode;
    }
    return false;
  }

  // Elements a keyboard user can reach with Tab inside root, in document order.
  function focusables(root) {
    return qsa('a, button, input, select, textarea, [tabindex]', root).filter(function (n) {
      if (n.disabled || n.tabIndex < 0) { return false; }
      if (n.localName === 'a' && !n.hasAttribute('href') && !n.hasAttribute('tabindex')) { return false; }
      return !isHidden(n, root);
    });
  }

  window.VP_Dom = {
    el: el,
    on: on,
    off: off,
    offOwner: offOwner,
    count: count,
    delegate: delegate,
    clear: clear,
    append: append,
    qs: qs,
    qsa: qsa,
    closest: closest,
    matches: matches,
    focusables: focusables
  };
}());
