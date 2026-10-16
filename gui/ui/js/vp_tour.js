/* vp_tour.js - the spotlight tour (PREDESIGN 1.6): a dimmed overlay with a hole over the
 * control a step talks about, and a card placed next to it (never on top of it).
 * Back / Next / Skip buttons; keyboard: Left/Right arrows, Escape skips, Tab stays in the
 * card. Each step's title gets focus so screen readers announce it. Motion is CSS only and
 * disappears under prefers-reduced-motion.
 *
 * VP_Tour.start(steps, {onDone(reason), onAction(step), startAt}) steps: [{target, titleKey,
 *   textKey, actionKey?}] (target: CSS selector; missing target = centred card without
 *   spotlight; actionKey adds a button to that step which ends the tour with reason
 *   "action" and calls onAction)
 * VP_Tour.next(), back(), skip(), stop(), isActive(), index()
 */
(function () {
  'use strict';

  var OWNER = 'tour';
  var GAP = 16;
  var PAD = 8;
  var tour = null;

  function px(n) { return Math.round(n) + 'px'; }

  function position() {
    if (!tour) { return; }
    var step = tour.steps[tour.i];
    var target = step.target ? document.querySelector(step.target) : null;
    var card = tour.card;
    var spot = tour.spot;
    var vw = window.innerWidth || 1280;
    var vh = window.innerHeight || 800;
    if (!target) {
      spot.hidden = true;
      card.className = 'vp-tour-card vp-tour-card-center';
      card.style.top = '';
      card.style.left = '';
      return;
    }
    if (typeof target.scrollIntoView === 'function') { target.scrollIntoView({ block: 'nearest' }); }
    var r = target.getBoundingClientRect();
    spot.hidden = false;
    spot.style.top = px(r.top - PAD);
    spot.style.left = px(r.left - PAD);
    spot.style.width = px(r.width + PAD * 2);
    spot.style.height = px(r.height + PAD * 2);
    card.className = 'vp-tour-card';
    var cw = card.offsetWidth || 360;
    var ch = card.offsetHeight || 200;
    var top = r.bottom + PAD + GAP;
    if (top + ch > vh && r.top - PAD - GAP - ch >= 0) { top = r.top - PAD - GAP - ch; }
    var left = Math.max(GAP, Math.min(r.left, vw - cw - GAP));
    card.style.top = px(Math.max(GAP, top));
    card.style.left = px(left);
  }

  function show(i) {
    var T = window.VP_I18n;
    tour.i = i;
    var step = tour.steps[i];
    var last = i === tour.steps.length - 1;
    tour.title.setAttribute('data-i18n', step.titleKey);
    tour.text.setAttribute('data-i18n', step.textKey);
    tour.counter.setAttribute('data-i18n-vars', JSON.stringify({ n: i + 1, total: tour.steps.length }));
    tour.next.setAttribute('data-i18n', last ? 'tour.done.cta' : 'tour.next.cta');
    tour.back.disabled = i === 0;
    tour.action.hidden = !step.actionKey;
    if (step.actionKey) { tour.action.setAttribute('data-i18n', step.actionKey); }
    T.bind(tour.root);
    position();
    tour.title.focus();
  }

  function finish(reason) {
    var done = tour && tour.onDone;
    stop();
    if (typeof done === 'function') { done(reason); }
  }

  function next() {
    if (!tour) { return; }
    if (tour.i >= tour.steps.length - 1) { finish('done'); } else { show(tour.i + 1); }
  }

  function back() {
    if (tour && tour.i > 0) { show(tour.i - 1); }
  }

  function skip() {
    if (tour) { finish('skip'); }
  }

  function action() {
    if (!tour) { return; }
    var fn = tour.onAction;
    var step = tour.steps[tour.i];
    finish('action');
    if (typeof fn === 'function') { fn(step); }
  }

  function stop() {
    if (!tour) { return; }
    var t = tour;
    tour = null;
    window.VP_Dom.offOwner(OWNER);
    if (t.root.parentNode) { t.root.parentNode.removeChild(t.root); }
    window.VP_Keys.resume();
    if (t.returnFocus && document.body.contains(t.returnFocus)) { t.returnFocus.focus(); }
  }

  function onKey(e) {
    if (!tour) { return; }
    if (e.key === 'Escape' || e.key === 'Esc') {
      e.preventDefault();
      skip();
    } else if (e.key === 'ArrowRight' && !window.VP_Keys.isTyping(e.target)) {
      e.preventDefault();
      next();
    } else if (e.key === 'ArrowLeft' && !window.VP_Keys.isTyping(e.target)) {
      e.preventDefault();
      back();
    } else if (e.key === 'Tab') {
      var list = window.VP_Dom.focusables(tour.card);
      if (!list.length) { return; }
      var first = list[0];
      var lastEl = list[list.length - 1];
      var active = document.activeElement;
      if (e.shiftKey && (active === first || !tour.card.contains(active) || active === tour.title)) {
        e.preventDefault();
        lastEl.focus();
      } else if (!e.shiftKey && (active === lastEl || !tour.card.contains(active))) {
        e.preventDefault();
        first.focus();
      }
    }
  }

  function start(steps, opts) {
    if (!steps || !steps.length) { throw new Error('VP_Tour.start: no steps'); }
    if (tour) { stop(); }
    opts = opts || {};
    var D = window.VP_Dom;
    var title = D.el('h2', { id: 'vp-tour-title', className: 'vp-tour-title', tabIndex: -1 });
    var text = D.el('p', { className: 'vp-tour-text' });
    var counter = D.el('p', { className: 'vp-tour-counter', 'data-i18n': 'tour.counter.label' });
    var backBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary', 'data-i18n': 'tour.back.cta', dataset: { tour: 'back' } });
    var nextBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-primary', dataset: { tour: 'next' } });
    var skipBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-tertiary', 'data-i18n': 'tour.skip.cta', dataset: { tour: 'skip' } });
    var actionBtn = D.el('button', { type: 'button', className: 'vp-btn vp-btn-secondary vp-tour-action', hidden: true, 'data-i18n': 'tour.done.cta', dataset: { tour: 'action' } });
    var card = D.el('div', { className: 'vp-tour-card' }, [
      counter, title, text,
      D.el('div', { className: 'vp-tour-actions' }, [skipBtn, D.el('span', { className: 'vp-spacer' }), actionBtn, backBtn, nextBtn])
    ]);
    var spot = D.el('div', { className: 'vp-tour-spot', hidden: true, 'aria-hidden': 'true' });
    var root = D.el('div', { className: 'vp-tour', role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': 'vp-tour-title' }, [spot, card]);
    tour = {
      steps: steps, i: 0, root: root, card: card, spot: spot, title: title, text: text, counter: counter,
      next: nextBtn, back: backBtn, action: actionBtn, onDone: opts.onDone, onAction: opts.onAction, returnFocus: document.activeElement
    };
    D.delegate(root, '[data-tour]', 'click', function (e, btn) {
      var a = btn.getAttribute('data-tour');
      if (a === 'next') { next(); } else if (a === 'back') { back(); } else if (a === 'action') { action(); } else { skip(); }
    }, { owner: OWNER });
    D.on(root, 'keydown', onKey, { owner: OWNER });
    D.on(window, 'resize', position, { owner: OWNER });
    window.VP_Keys.suspend();
    document.body.appendChild(root);
    show(Math.max(0, Math.min(steps.length - 1, opts.startAt || 0)));
  }

  window.VP_Tour = {
    start: start,
    next: next,
    back: back,
    skip: skip,
    stop: stop,
    reposition: position,
    isActive: function () { return tour !== null; },
    index: function () { return tour ? tour.i : -1; }
  };
}());
