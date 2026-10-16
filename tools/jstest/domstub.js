/* domstub.js - a small fake browser for running the ES5 UI modules inside a Node vm context.
 *
 * Not a DOM implementation: just enough tree, attribute, selector, event, focus, timer, storage
 * and XHR behaviour for unit tests, plus recorders the tests read (listener count, timer count,
 * innerHTML writes). Anything unsupported throws, so a test never passes by accident.
 *
 * createEnv(opts) -> env
 *   opts.search    location.search (e.g. '?mock=1')
 *   opts.webview   true: window.chrome.webview exists (postMessage recorder + 'message' events)
 *   opts.files     {url: text} answered by XMLHttpRequest; opts.root: directory for other URLs
 *   opts.media     {query: bool} answers for matchMedia
 * env.window, env.document, env.clock.{now,tick,flush,runAll,pending}, env.listenerCount(),
 * env.fire(target, type, props), env.key(target, key, mods), env.logs, env.webviewSent
 */
'use strict';

var fs = require('fs');
var path = require('path');

function kebab(name) {
  return name.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
}
function camel(name) {
  return name.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
}

// ------------------------------------------------------------------ selectors
function parseCompound(text) {
  var c = { tag: null, id: null, classes: [], attrs: [] };
  var re = /^(\*|[a-zA-Z][\w-]*)|#([\w-]+)|\.([\w-]+)|\[\s*([\w-]+)\s*(?:([~^$*|]?=)\s*(?:"([^"]*)"|'([^']*)'|([^\]\s]+)))?\s*\]/y;
  var i = 0;
  var first = true;
  while (i < text.length) {
    re.lastIndex = i;
    var m = re.exec(text);
    if (!m || m.index !== i || (m[1] && !first)) { throw new Error('domstub: unsupported selector "' + text + '"'); }
    if (m[1] && m[1] !== '*') { c.tag = m[1].toLowerCase(); }
    if (m[2]) { c.id = m[2]; }
    if (m[3]) { c.classes.push(m[3]); }
    if (m[4]) {
      var v = m[6] !== undefined ? m[6] : (m[7] !== undefined ? m[7] : m[8]);
      c.attrs.push({ name: m[4].toLowerCase(), op: m[5] || null, value: v });
    }
    i = re.lastIndex;
    first = false;
  }
  return c;
}

function parseSelector(sel) {
  return sel.split(',').map(function (group) {
    var parts = [];
    var tokens = group.trim().replace(/\s*>\s*/g, ' > ').split(/\s+/);
    var comb = ' ';
    tokens.forEach(function (t) {
      if (t === '>') { comb = '>'; return; }
      if (t === '') { return; }
      parts.push({ comb: comb, c: parseCompound(t) });
      comb = ' ';
    });
    if (!parts.length) { throw new Error('domstub: empty selector'); }
    return parts;
  });
}

function matchCompound(el, c) {
  if (el.nodeType !== 1) { return false; }
  if (c.tag && el.localName !== c.tag) { return false; }
  if (c.id && el.getAttribute('id') !== c.id) { return false; }
  for (var i = 0; i < c.classes.length; i++) {
    if (!el.classList.contains(c.classes[i])) { return false; }
  }
  for (var j = 0; j < c.attrs.length; j++) {
    var a = c.attrs[j];
    if (!el.hasAttribute(a.name)) { return false; }
    var val = el.getAttribute(a.name);
    if (a.op === '=' && val !== a.value) { return false; }
    if (a.op === '~=' && (' ' + val + ' ').indexOf(' ' + a.value + ' ') < 0) { return false; }
    if (a.op === '^=' && val.indexOf(a.value) !== 0) { return false; }
    if (a.op === '*=' && val.indexOf(a.value) < 0) { return false; }
    if (a.op === '$=' && val.slice(-a.value.length) !== a.value) { return false; }
  }
  return true;
}

function matchParts(el, parts, idx) {
  if (!matchCompound(el, parts[idx].c)) { return false; }
  if (idx === 0) { return true; }
  var comb = parts[idx].comb;
  var p = el.parentNode;
  if (comb === '>') { return !!p && p.nodeType === 1 && matchParts(p, parts, idx - 1); }
  while (p && p.nodeType === 1) {
    if (matchParts(p, parts, idx - 1)) { return true; }
    p = p.parentNode;
  }
  return false;
}

function matches(el, sel) {
  var groups = parseSelector(sel);
  for (var g = 0; g < groups.length; g++) {
    if (matchParts(el, groups[g], groups[g].length - 1)) { return true; }
  }
  return false;
}

// ------------------------------------------------------------------ env
function createEnv(opts) {
  opts = opts || {};
  var env = { logs: [], innerHTMLWrites: 0, webviewSent: [] };
  var listenerTotal = 0;

  // ---------------------------------------------------------------- clock
  var clock = { t: 1000, seq: 0, timers: [] };
  function addTimer(fn, ms, interval, kind) {
    if (typeof fn !== 'function') { throw new Error('domstub: timer callback must be a function'); }
    ms = Math.max(0, Number(ms) || 0);
    var id = ++clock.seq;
    clock.timers.push({ id: id, at: clock.t + ms, fn: fn, interval: interval ? Math.max(1, ms) : 0, seq: id, kind: kind });
    return id;
  }
  function removeTimer(id) {
    for (var i = 0; i < clock.timers.length; i++) {
      if (clock.timers[i].id === id) { clock.timers.splice(i, 1); return; }
    }
  }
  function nextDue(limit) {
    var best = null;
    for (var i = 0; i < clock.timers.length; i++) {
      var tm = clock.timers[i];
      if (tm.at <= limit && (!best || tm.at < best.at || (tm.at === best.at && tm.seq < best.seq))) { best = tm; }
    }
    return best;
  }
  function tick(ms) {
    var target = clock.t + (ms || 0);
    var guard = 0;
    var tm;
    while ((tm = nextDue(target)) !== null) {
      if (++guard > 200000) { throw new Error('domstub: runaway timers'); }
      clock.t = Math.max(clock.t, tm.at);
      if (tm.interval) { tm.at = clock.t + tm.interval; tm.seq = ++clock.seq; } else { removeTimer(tm.id); }
      if (tm.kind === 'raf') { tm.fn(clock.t); } else { tm.fn(); }
    }
    clock.t = target;
  }
  env.clock = {
    now: function () { return clock.t; },
    tick: tick,
    flush: function () { tick(0); },
    runAll: function (maxMs) {
      var limit = clock.t + (maxMs || 600000);
      while (clock.timers.length) {
        var due = nextDue(limit);
        if (!due) { break; }
        tick(due.at - clock.t);
      }
    },
    pending: function () { return clock.timers.length; }
  };

  // ---------------------------------------------------------------- events
  function Event(type, props) {
    this.type = type;
    this.bubbles = true;
    this.cancelable = true;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this._stop = false;
    this._stopNow = false;
    this.timeStamp = clock.t;
    if (props) { for (var k in props) { if (Object.prototype.hasOwnProperty.call(props, k)) { this[k] = props[k]; } } }
  }
  Event.prototype.preventDefault = function () { if (this.cancelable) { this.defaultPrevented = true; } };
  Event.prototype.stopPropagation = function () { this._stop = true; };
  Event.prototype.stopImmediatePropagation = function () { this._stop = true; this._stopNow = true; };

  function EventTargetMixin(obj) {
    obj._listeners = [];
    obj.addEventListener = function (type, fn, capture) {
      if (typeof fn !== 'function') { return; }
      var cap = !!(capture && (capture === true || capture.capture));
      for (var i = 0; i < this._listeners.length; i++) {
        var l = this._listeners[i];
        if (l.type === type && l.fn === fn && l.capture === cap) { return; }
      }
      this._listeners.push({ type: type, fn: fn, capture: cap });
      listenerTotal++;
    };
    obj.removeEventListener = function (type, fn, capture) {
      var cap = !!(capture && (capture === true || capture.capture));
      for (var i = 0; i < this._listeners.length; i++) {
        var l = this._listeners[i];
        if (l.type === type && l.fn === fn && l.capture === cap) {
          this._listeners.splice(i, 1);
          listenerTotal--;
          return;
        }
      }
    };
    obj.dispatchEvent = function (ev) { return dispatch(this, ev); };
  }

  function invoke(node, ev, phase) {
    var list = node._listeners.slice();
    ev.currentTarget = node;
    for (var i = 0; i < list.length; i++) {
      var l = list[i];
      if (l.type !== ev.type) { continue; }
      if (phase === 'capture' && !l.capture) { continue; }
      if (phase === 'bubble' && l.capture) { continue; }
      if (node._listeners.indexOf(l) < 0) { continue; }
      l.fn.call(node, ev);
      if (ev._stopNow) { break; }
    }
  }

  function dispatch(target, ev) {
    if (!(ev instanceof Event)) { ev = new Event(ev.type, ev); }
    ev.target = target;
    var pathList = [];
    var p = target.parentNode || null;
    while (p) { pathList.push(p); p = p.parentNode || null; }
    if (target === win) { pathList = []; } else if (pathList.length && pathList[pathList.length - 1] === doc) { pathList.push(win); } else if (target === doc) { pathList.push(win); }
    var i;
    for (i = pathList.length - 1; i >= 0 && !ev._stop; i--) { invoke(pathList[i], ev, 'capture'); }
    if (!ev._stop) { invoke(target, ev, 'target'); }
    if (ev.bubbles) {
      for (i = 0; i < pathList.length && !ev._stop; i++) { invoke(pathList[i], ev, 'bubble'); }
    }
    ev.currentTarget = null;
    return !ev.defaultPrevented;
  }

  // ---------------------------------------------------------------- nodes
  var FOCUSABLE_TAGS = { a: 1, button: 1, input: 1, select: 1, textarea: 1 };

  function Node(nodeType, name) {
    this.nodeType = nodeType;
    this.childNodes = [];
    this.parentNode = null;
    if (nodeType === 1) {
      this.localName = name.toLowerCase();
      this.tagName = name.toUpperCase();
      this.nodeName = this.tagName;
      this._attrs = {};
      this._attrOrder = [];
      this._style = makeStyle();
      this._html = '';
      this.value = '';
      this.checked = false;
      this._rect = null;
      var self = this;
      this.dataset = new Proxy({}, {
        get: function (t, key) { return typeof key === 'string' ? (self.getAttribute('data-' + kebab(key)) === null ? undefined : self.getAttribute('data-' + kebab(key))) : undefined; },
        set: function (t, key, v) { self.setAttribute('data-' + kebab(key), String(v)); return true; },
        deleteProperty: function (t, key) { self.removeAttribute('data-' + kebab(key)); return true; },
        has: function (t, key) { return self.hasAttribute('data-' + kebab(key)); },
        ownKeys: function () { return self._attrOrder.filter(function (a) { return a.indexOf('data-') === 0; }).map(function (a) { return camel(a.slice(5)); }); },
        getOwnPropertyDescriptor: function (t, key) {
          var v = self.getAttribute('data-' + kebab(key));
          return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true };
        }
      });
      this.classList = makeClassList(this);
    } else if (nodeType === 3) {
      this.nodeName = '#text';
      this.data = name;
    } else if (nodeType === 11) {
      this.nodeName = '#document-fragment';
    }
    EventTargetMixin(this);
  }

  function makeStyle() {
    var st = {};
    Object.defineProperty(st, 'setProperty', { value: function (k, v) { st[k] = String(v); } });
    Object.defineProperty(st, 'getPropertyValue', { value: function (k) { return st[k] === undefined ? '' : st[k]; } });
    Object.defineProperty(st, 'removeProperty', { value: function (k) { var o = st[k]; delete st[k]; return o === undefined ? '' : o; } });
    return st;
  }

  function makeClassList(el) {
    function list() { var c = el.getAttribute('class'); return c ? c.split(/\s+/).filter(Boolean) : []; }
    function write(arr) { el.setAttribute('class', arr.join(' ')); }
    return {
      add: function () { var a = list(); for (var i = 0; i < arguments.length; i++) { if (a.indexOf(arguments[i]) < 0) { a.push(arguments[i]); } } write(a); },
      remove: function () { var a = list(); for (var i = 0; i < arguments.length; i++) { var k = a.indexOf(arguments[i]); if (k >= 0) { a.splice(k, 1); } } write(a); },
      contains: function (c) { return list().indexOf(c) >= 0; },
      toggle: function (c, force) {
        var has = list().indexOf(c) >= 0;
        var want = force === undefined ? !has : !!force;
        if (want && !has) { this.add(c); } else if (!want && has) { this.remove(c); }
        return want;
      },
      item: function (i) { return list()[i] || null; },
      get length() { return list().length; },
      toString: function () { return list().join(' '); }
    };
  }

  function attrProp(prop, attr, kind) {
    Object.defineProperty(Node.prototype, prop, {
      get: function () {
        var v = this.getAttribute(attr);
        if (kind === 'bool') { return v !== null; }
        return v === null ? '' : v;
      },
      set: function (v) {
        if (kind === 'bool') { if (v) { this.setAttribute(attr, ''); } else { this.removeAttribute(attr); } } else { this.setAttribute(attr, String(v)); }
      },
      configurable: true
    });
  }
  attrProp('id', 'id');
  attrProp('className', 'class');
  attrProp('title', 'title');
  attrProp('placeholder', 'placeholder');
  attrProp('lang', 'lang');
  attrProp('type', 'type');
  attrProp('href', 'href');
  attrProp('src', 'src');
  attrProp('name', 'name');
  attrProp('role', 'role');
  attrProp('htmlFor', 'for');
  attrProp('hidden', 'hidden', 'bool');
  attrProp('disabled', 'disabled', 'bool');

  Object.defineProperty(Node.prototype, 'tabIndex', {
    get: function () {
      var v = this.getAttribute('tabindex');
      if (v !== null) { return parseInt(v, 10); }
      if (this.localName === 'a') { return this.hasAttribute('href') ? 0 : -1; }
      return FOCUSABLE_TAGS[this.localName] ? 0 : -1;
    },
    set: function (v) { this.setAttribute('tabindex', String(v)); }
  });
  Object.defineProperty(Node.prototype, 'style', {
    get: function () { return this._style; },
    set: function () { throw new Error('domstub: assigning element.style is not supported (CSP); set properties instead'); }
  });
  Object.defineProperty(Node.prototype, 'textContent', {
    get: function () {
      if (this.nodeType === 3) { return this.data; }
      var s = '';
      for (var i = 0; i < this.childNodes.length; i++) { s += this.childNodes[i].textContent; }
      return s;
    },
    set: function (v) {
      if (this.nodeType === 3) { this.data = String(v); return; }
      while (this.childNodes.length) { this.removeChild(this.childNodes[0]); }
      v = v === null || v === undefined ? '' : String(v);
      if (v !== '') { this.appendChild(new Node(3, v)); }
    }
  });
  Object.defineProperty(Node.prototype, 'nodeValue', {
    get: function () { return this.nodeType === 3 ? this.data : null; },
    set: function (v) { if (this.nodeType === 3) { this.data = String(v); } }
  });
  Object.defineProperty(Node.prototype, 'innerHTML', {
    get: function () { return this._html; },
    set: function (v) {
      env.innerHTMLWrites++;
      this._html = String(v);
      while (this.childNodes.length) { this.removeChild(this.childNodes[0]); }
    }
  });
  Object.defineProperty(Node.prototype, 'children', { get: function () { return this.childNodes.filter(function (n) { return n.nodeType === 1; }); } });
  Object.defineProperty(Node.prototype, 'childElementCount', { get: function () { return this.children.length; } });
  Object.defineProperty(Node.prototype, 'firstChild', { get: function () { return this.childNodes[0] || null; } });
  Object.defineProperty(Node.prototype, 'lastChild', { get: function () { return this.childNodes[this.childNodes.length - 1] || null; } });
  Object.defineProperty(Node.prototype, 'firstElementChild', { get: function () { return this.children[0] || null; } });
  Object.defineProperty(Node.prototype, 'lastElementChild', { get: function () { var c = this.children; return c[c.length - 1] || null; } });
  Object.defineProperty(Node.prototype, 'parentElement', { get: function () { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; } });
  Object.defineProperty(Node.prototype, 'ownerDocument', { get: function () { return doc; } });
  function sibling(node, step, elementsOnly) {
    if (!node.parentNode) { return null; }
    var list = node.parentNode.childNodes;
    for (var i = list.indexOf(node) + step; i >= 0 && i < list.length; i += step) {
      if (!elementsOnly || list[i].nodeType === 1) { return list[i]; }
    }
    return null;
  }
  Object.defineProperty(Node.prototype, 'nextSibling', { get: function () { return sibling(this, 1, false); } });
  Object.defineProperty(Node.prototype, 'previousSibling', { get: function () { return sibling(this, -1, false); } });
  Object.defineProperty(Node.prototype, 'nextElementSibling', { get: function () { return sibling(this, 1, true); } });
  Object.defineProperty(Node.prototype, 'previousElementSibling', { get: function () { return sibling(this, -1, true); } });
  Object.defineProperty(Node.prototype, 'isConnected', {
    get: function () { var p = this; while (p) { if (p === doc) { return true; } p = p.parentNode; } return false; }
  });
  Object.defineProperty(Node.prototype, 'offsetWidth', { get: function () { return this._rect ? this._rect.width : 0; } });
  Object.defineProperty(Node.prototype, 'offsetHeight', { get: function () { return this._rect ? this._rect.height : 0; } });

  Node.prototype.setAttribute = function (name, value) {
    name = String(name).toLowerCase();
    if (name === 'style') { throw new Error('domstub: setAttribute("style") is blocked by the CSP'); }
    if (!(name in this._attrs)) { this._attrOrder.push(name); }
    this._attrs[name] = String(value);
  };
  Node.prototype.getAttribute = function (name) {
    name = String(name).toLowerCase();
    return Object.prototype.hasOwnProperty.call(this._attrs, name) ? this._attrs[name] : null;
  };
  Node.prototype.hasAttribute = function (name) { return this.getAttribute(name) !== null; };
  Node.prototype.removeAttribute = function (name) {
    name = String(name).toLowerCase();
    if (Object.prototype.hasOwnProperty.call(this._attrs, name)) {
      delete this._attrs[name];
      this._attrOrder.splice(this._attrOrder.indexOf(name), 1);
    }
  };
  Object.defineProperty(Node.prototype, 'attributes', {
    get: function () { var self = this; return this._attrOrder.map(function (n) { return { name: n, value: self._attrs[n] }; }); }
  });

  function detach(child) {
    if (child.parentNode) {
      var list = child.parentNode.childNodes;
      list.splice(list.indexOf(child), 1);
      child.parentNode = null;
    }
  }
  function checkInsert(parent, child) {
    if (!child || typeof child.nodeType !== 'number') { throw new Error('domstub: not a node'); }
    var p = parent;
    while (p) { if (p === child) { throw new Error('domstub: HierarchyRequestError'); } p = p.parentNode; }
  }
  Node.prototype.appendChild = function (child) { return this.insertBefore(child, null); };
  Node.prototype.insertBefore = function (child, ref) {
    checkInsert(this, child);
    if (child.nodeType === 11) {
      var kids = child.childNodes.slice();
      for (var i = 0; i < kids.length; i++) { this.insertBefore(kids[i], ref); }
      return child;
    }
    detach(child);
    var idx = ref ? this.childNodes.indexOf(ref) : -1;
    if (ref && idx < 0) { throw new Error('domstub: NotFoundError (insertBefore ref)'); }
    if (idx < 0) { this.childNodes.push(child); } else { this.childNodes.splice(idx, 0, child); }
    child.parentNode = this;
    return child;
  };
  Node.prototype.removeChild = function (child) {
    if (child.parentNode !== this) { throw new Error('domstub: NotFoundError (removeChild)'); }
    detach(child);
    if (doc && doc._active && (doc._active === child || child.contains(doc._active))) { doc._active = null; }
    return child;
  };
  Node.prototype.replaceChild = function (newChild, oldChild) {
    this.insertBefore(newChild, oldChild);
    return this.removeChild(oldChild);
  };
  Node.prototype.remove = function () { if (this.parentNode) { this.parentNode.removeChild(this); } };
  Node.prototype.hasChildNodes = function () { return this.childNodes.length > 0; };
  Node.prototype.contains = function (other) {
    while (other) { if (other === this) { return true; } other = other.parentNode; }
    return false;
  };
  function walk(root, fn) {
    for (var i = 0; i < root.childNodes.length; i++) {
      var n = root.childNodes[i];
      if (n.nodeType === 1) { fn(n); }
      walk(n, fn);
    }
  }
  Node.prototype.querySelectorAll = function (sel) {
    var out = [];
    var groups = parseSelector(sel);
    walk(this, function (n) {
      for (var g = 0; g < groups.length; g++) {
        if (matchParts(n, groups[g], groups[g].length - 1)) { out.push(n); return; }
      }
    });
    return out;
  };
  Node.prototype.querySelector = function (sel) { return this.querySelectorAll(sel)[0] || null; };
  Node.prototype.getElementsByTagName = function (tag) {
    tag = tag.toLowerCase();
    var out = [];
    walk(this, function (n) { if (tag === '*' || n.localName === tag) { out.push(n); } });
    return out;
  };
  Node.prototype.getElementsByClassName = function (cls) {
    var out = [];
    walk(this, function (n) { if (n.classList.contains(cls)) { out.push(n); } });
    return out;
  };
  Node.prototype.matches = function (sel) { return matches(this, sel); };
  Node.prototype.closest = function (sel) {
    var n = this;
    while (n && n.nodeType === 1) { if (matches(n, sel)) { return n; } n = n.parentNode; }
    return null;
  };
  Node.prototype.focus = function () {
    if (!this.isConnected) { return; }
    var prev = doc.activeElement;
    if (prev === this) { return; }
    doc._active = this;
    if (prev && prev !== doc.body) { dispatch(prev, new Event('blur', { bubbles: false })); dispatch(prev, new Event('focusout')); }
    dispatch(this, new Event('focus', { bubbles: false }));
    dispatch(this, new Event('focusin'));
  };
  Node.prototype.blur = function () { if (doc._active === this) { doc._active = null; dispatch(this, new Event('blur', { bubbles: false })); } };
  Node.prototype.click = function () { if (!this.disabled) { dispatch(this, new Event('click')); } };
  Node.prototype.getBoundingClientRect = function () {
    var r = this._rect || { top: 0, left: 0, width: 0, height: 0 };
    return { top: r.top, left: r.left, width: r.width, height: r.height, right: r.left + r.width, bottom: r.top + r.height, x: r.left, y: r.top };
  };
  Node.prototype.scrollIntoView = function () {};
  Node.prototype.cloneNode = function () { throw new Error('domstub: cloneNode is not supported'); };

  // ---------------------------------------------------------------- document
  var doc = new Node(9, '#document');
  doc.nodeName = '#document';
  var html = new Node(1, 'html');
  var head = new Node(1, 'head');
  var body = new Node(1, 'body');
  html.setAttribute('lang', 'en-US');
  doc.childNodes.push(html); html.parentNode = doc;
  html.appendChild(head);
  html.appendChild(body);
  doc.documentElement = html;
  doc.head = head;
  doc.body = body;
  doc._active = null;
  Object.defineProperty(doc, 'title', { value: '', writable: true });
  doc.readyState = 'complete';
  Object.defineProperty(doc, 'activeElement', { get: function () { return doc._active && doc._active.isConnected ? doc._active : body; } });
  doc.createElement = function (tag) { return new Node(1, String(tag)); };
  doc.createElementNS = function (ns, tag) { var n = new Node(1, String(tag)); n.namespaceURI = ns; return n; };
  doc.createTextNode = function (text) { return new Node(3, String(text)); };
  doc.createDocumentFragment = function () { return new Node(11, '#document-fragment'); };
  doc.getElementById = function (id) { return html.querySelector('#' + id) || null; };
  doc.createEvent = function () { return new Event('custom'); };

  // ---------------------------------------------------------------- window
  var win = {};
  EventTargetMixin(win);
  win.window = win;
  win.self = win;
  win.document = doc;
  win.Event = Event;
  win.KeyboardEvent = Event;
  win.innerWidth = 1280;
  win.innerHeight = 800;
  win.devicePixelRatio = 1;
  win.location = {
    search: opts.search || '',
    hash: '',
    protocol: 'http:',
    href: 'http://127.0.0.1/index.html' + (opts.search || ''),
    pathname: '/index.html',
    reload: function () {}
  };
  win.navigator = { userAgent: 'domstub', language: opts.language || 'en-US', languages: [opts.language || 'en-US'] };
  function storage() {
    var data = {};
    return {
      getItem: function (k) { return Object.prototype.hasOwnProperty.call(data, k) ? data[k] : null; },
      setItem: function (k, v) { data[k] = String(v); },
      removeItem: function (k) { delete data[k]; },
      clear: function () { data = {}; },
      key: function (i) { return Object.keys(data)[i] || null; },
      get length() { return Object.keys(data).length; }
    };
  }
  win.localStorage = storage();
  win.sessionStorage = storage();
  win.setTimeout = function (fn, ms) { return addTimer(fn, ms, false, 'timeout'); };
  win.clearTimeout = function (id) { removeTimer(id); };
  win.setInterval = function (fn, ms) { return addTimer(fn, ms, true, 'interval'); };
  win.clearInterval = function (id) { removeTimer(id); };
  win.requestAnimationFrame = function (fn) { return addTimer(fn, 16, false, 'raf'); };
  win.cancelAnimationFrame = function (id) { removeTimer(id); };
  win.performance = { now: function () { return clock.t; } };
  win.getComputedStyle = function (el) { return el.style; };
  win.scrollTo = function () {};
  var media = opts.media || {};
  win.matchMedia = function (q) {
    return {
      media: q,
      matches: !!media[q],
      addListener: function () { listenerTotal++; },
      removeListener: function () { listenerTotal--; },
      addEventListener: function () { listenerTotal++; },
      removeEventListener: function () { listenerTotal--; }
    };
  };
  function logger(level) {
    return function () {
      var parts = [];
      for (var i = 0; i < arguments.length; i++) {
        var a = arguments[i];
        parts.push(a && a.stack ? String(a.stack).split('\n')[0] : (typeof a === 'object' ? safeJson(a) : String(a)));
      }
      env.logs.push({ level: level, text: parts.join(' ') });
      if (opts.echo) { process.stdout.write('    [' + level + '] ' + parts.join(' ') + '\n'); }
    };
  }
  function safeJson(o) { try { return JSON.stringify(o); } catch (e) { return String(o); } }
  win.console = { log: logger('log'), info: logger('info'), warn: logger('warn'), error: logger('error'), debug: logger('debug') };

  // XMLHttpRequest answering from opts.files or from opts.root on disk.
  var files = opts.files || {};
  function XHR() {
    this.readyState = 0;
    this.status = 0;
    this.responseText = '';
    this.onload = null;
    this.onerror = null;
    this.onreadystatechange = null;
    this._url = null;
  }
  XHR.prototype.open = function (method, url) { this._method = method; this._url = String(url); this.readyState = 1; };
  XHR.prototype.overrideMimeType = function () {};
  XHR.prototype.setRequestHeader = function () {};
  XHR.prototype.abort = function () { this._aborted = true; };
  XHR.prototype.send = function () {
    var self = this;
    var url = this._url.replace(/^\.\//, '').split('?')[0];
    addTimer(function () {
      if (self._aborted) { return; }
      var text = null;
      if (Object.prototype.hasOwnProperty.call(files, url)) { text = files[url]; } else if (opts.root) {
        try { text = fs.readFileSync(path.join(opts.root, url), 'utf8'); } catch (e) { text = null; }
      }
      self.readyState = 4;
      if (text === null) { self.status = opts.fileStatus0 ? 0 : 404; } else { self.status = 200; self.responseText = text; }
      if (self.onreadystatechange) { self.onreadystatechange(); }
      if (text === null && opts.fileStatus0 && self.onerror) { self.onerror(); return; }
      if (self.onload) { self.onload(); }
    }, 0, false, 'xhr');
  };
  win.XMLHttpRequest = XHR;

  if (opts.webview) {
    var wv = {};
    EventTargetMixin(wv);
    wv.postMessage = function (msg) { env.webviewSent.push(msg); };
    win.chrome = { webview: wv };
    env.webviewReply = function (obj) {
      dispatch(wv, new Event('message', { data: typeof obj === 'string' ? obj : JSON.stringify(obj), bubbles: false }));
    };
  }

  env.window = win;
  env.document = doc;
  env.Event = Event;
  env.listenerCount = function () { return listenerTotal; };
  env.fire = function (target, type, props) { return dispatch(target, new Event(type, props)); };
  env.key = function (target, key, mods) {
    mods = mods || {};
    return dispatch(target, new Event('keydown', {
      key: key, ctrlKey: !!mods.ctrl, shiftKey: !!mods.shift, altKey: !!mods.alt, metaKey: !!mods.meta
    }));
  };
  env.setRect = function (el, r) { el._rect = { top: r.top || 0, left: r.left || 0, width: r.width || 0, height: r.height || 0 }; };
  env.errors = function () { return env.logs.filter(function (l) { return l.level === 'error'; }); };
  return env;
}

module.exports = { createEnv: createEnv, matches: matches, parseSelector: parseSelector };
