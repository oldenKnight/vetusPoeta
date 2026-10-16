describe('VP_Tour', function () {
  function setup() {
    var env = load(['vp_dom.js', 'vp_timers.js', 'vp_i18n.js', 'vp_keys.js', 'vp_tour.js']);
    env.window.VP_I18n.load('en-US', {
      's1.title': 'First', 's1.text': 'One', 's2.title': 'Second', 's2.text': 'Two', 's3.title': 'Third', 's3.text': 'Three',
      'tour.counter.label': 'Step {n} of {total}', 'tour.next.cta': 'Next', 'tour.back.cta': 'Back', 'tour.done.cta': 'Done', 'tour.skip.cta': 'Skip tour'
    });
    var D = env.window.VP_Dom;
    env.a = D.el('button', { id: 'a' });
    env.b = D.el('button', { id: 'b' });
    env.document.body.appendChild(D.el('div', null, [env.a, env.b]));
    env.setRect(env.a, { top: 100, left: 50, width: 200, height: 40 });
    env.setRect(env.b, { top: 700, left: 1200, width: 60, height: 40 });
    env.a.focus();
    env.steps = [
      { target: '#a', titleKey: 's1.title', textKey: 's1.text' },
      { target: '#b', titleKey: 's2.title', textKey: 's2.text' },
      { target: '#missing', titleKey: 's3.title', textKey: 's3.text' }
    ];
    return env;
  }
  function q(env, sel) { return env.document.querySelector(sel); }

  it('spotlights the target, puts the card beside it and focuses the step title', function () {
    var env = setup();
    env.window.VP_Tour.start(env.steps);
    var root = q(env, '.vp-tour');
    eq(root.getAttribute('role'), 'dialog');
    eq(root.getAttribute('aria-modal'), 'true');
    eq(q(env, '.vp-tour-counter').textContent, 'Step 1 of 3');
    eq(q(env, '.vp-tour-title').textContent, 'First');
    eq(env.document.activeElement, q(env, '.vp-tour-title'));
    var spot = q(env, '.vp-tour-spot');
    eq(spot.hidden, false);
    eq(spot.style.top, '92px');
    eq(spot.style.left, '42px');
    eq(spot.style.width, '216px');
    eq(q(env, '.vp-tour-card').style.top, '164px', 'card below the target');
    ok(q(env, '[data-tour="back"]').disabled);
    ok(env.window.VP_Keys.isSuspended());
  });

  it('Next / Back / arrows move; the card goes above a low target and centres without one', function () {
    var env = setup();
    var T = env.window.VP_Tour;
    T.start(env.steps);
    env.key(q(env, '.vp-tour-title'), 'ArrowRight');
    eq(T.index(), 1);
    var card = q(env, '.vp-tour-card');
    ok(parseInt(card.style.top, 10) < 700, 'card above: ' + card.style.top);
    ok(parseInt(card.style.left, 10) <= 1280 - 360 - 16, 'card kept on screen: ' + card.style.left);
    q(env, '[data-tour="next"]').click();
    eq(T.index(), 2);
    eq(q(env, '.vp-tour-spot').hidden, true);
    ok(card.classList.contains('vp-tour-card-center'));
    eq(q(env, '[data-tour="next"]').textContent, 'Done');
    q(env, '[data-tour="back"]').click();
    env.key(q(env, '.vp-tour-title'), 'ArrowLeft');
    eq(T.index(), 0);
  });

  it('Escape skips, the last Next finishes; everything it added is removed', function () {
    var env = setup();
    var T = env.window.VP_Tour;
    var reasons = [];
    T.start(env.steps, { onDone: function (r) { reasons.push(r); } });
    env.key(q(env, '.vp-tour-title'), 'Escape');
    T.start(env.steps, { onDone: function (r) { reasons.push(r); }, startAt: 2 });
    q(env, '[data-tour="next"]').click();
    deepEq(reasons, ['skip', 'done']);
    eq(T.isActive(), false);
    eq(q(env, '.vp-tour'), null);
    eq(env.window.VP_Dom.count(), 0);
    eq(env.listenerCount(), 0);
    eq(env.window.VP_Keys.isSuspended(), false);
    eq(env.document.activeElement, env.a, 'focus returned');
  });

  it('keeps Tab inside the card', function () {
    var env = setup();
    env.window.VP_Tour.start(env.steps, { startAt: 1 });
    var list = env.window.VP_Dom.focusables(q(env, '.vp-tour-card'));
    list[list.length - 1].focus();
    env.key(env.document.activeElement, 'Tab');
    eq(env.document.activeElement, list[0]);
    env.window.VP_Tour.stop();
  });
});
