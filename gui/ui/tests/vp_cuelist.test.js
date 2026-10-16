describe('VP_CueList', function () {
  function setup() {
    var env = load('all');
    var W = env.window;
    W.VP_I18n.load('en-US', readJson('i18n/en-US.json'));
    W.VP_I18n.load('es-MX', readJson('i18n/es-MX.json'));
    W.VP_I18n.setLang('en-US');
    W.VP_Debug.enable(true);
    W.VP_MockEngine.options.latencyMs = 1;
    W.VP_Bridge.init({ mock: true });
    W.VP_Keys.bind(env.document);
    env.call = function (cmd, params) {
      var out = { result: null, error: null };
      W.VP_Bridge.call(cmd, params).then(function (r) { out.result = r; }, function (e) { out.error = e; });
      env.clock.tick(20);
      return out;
    };
    W.VP_Store.set('settings', env.call('settings.get').result);
    env.box = W.VP_Dom.el('div');
    env.document.body.appendChild(env.box);
    return env;
  }
  function open(env, params) {
    var r = params.path ? env.call('project.open', params) : env.call('project.new', params);
    var p = env.window.VP_Start.normalizeProject(r.result.project);
    env.window.VP_Store.set('project', p);
    return p;
  }
  function mountList(env, total, extra) {
    var W = env.window;
    var opts = { total: total, kind: 'subs', pair: 'en-la' };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { opts[k] = extra[k]; } } }
    W.VP_CueList.mount(env.box, opts);
    env.clock.tick(50);
    return W.VP_CueList;
  }
  function rows(env) { return env.document.querySelectorAll('.vp-cue-row'); }
  function viewport(env) { return env.document.querySelector('.vp-cl-viewport'); }
  function scrollTo(env, top) {
    var v = viewport(env);
    v.scrollTop = top;
    env.fire(v, 'scroll', { bubbles: false });
    env.clock.tick(20);
  }

  it('window arithmetic: never more than 40 rows, always covers the viewport (50,000 cues)', function () {
    var C = setup().window.VP_CueList;
    var n = 50000;
    var heights = [0, 1, 47, 48, 300, 640, 799, 1080, 1919, 2400, 5000];
    var tops = [0, 1, 47, 48, 1000, 123456, 48 * 25000 + 13, 48 * (n - 15), 48 * n, 48 * n + 999, 1e9];
    for (var t = 0; t < 2000; t++) { tops.push(Math.floor(Math.abs(Math.sin(t + 1)) * 48 * n)); }
    heights.forEach(function (h) {
      tops.forEach(function (top) {
        var w = C.windowFor(top, h, n);
        ok(w.to - w.from <= 40, 'at most 40 rows (' + h + ', ' + top + '): ' + (w.to - w.from));
        ok(w.from >= 0 && w.to <= n && w.from < w.to, 'inside the list');
        var need = Math.min(40, Math.ceil(h / 48) + 1);
        var first = Math.min(Math.floor(top / 48), n - need);
        ok(w.from <= first && Math.min(n, first + need) <= w.to, 'covers the visible rows (' + h + ', ' + top + ') ' + JSON.stringify(w));
      });
    });
    var mid = C.windowFor(48 * 1000, 640, n);
    deepEq([mid.from, mid.to], [992, 1023], '15 visible rows plus 8 above and 8 below');
    deepEq(C.windowFor(0, 640, 5), { from: 0, to: 5, first: 0, visible: 15 });
    deepEq(C.windowFor(500, 640, 0), { from: 0, to: 0, first: 0, visible: 15 });
  });

  it('mounts 50,000 cues: at most 40 rendered rows at any scroll offset, ARIA set and position, background load of every cue', function () {
    var env = setup();
    var W = env.window;
    var p = open(env, { path: 'C:\\x\\demo-50000-cues.vpoeta' });
    eq(p.cues, 50000);
    var L = mountList(env, p.cues);
    var vp = viewport(env);
    eq(vp.getAttribute('role'), 'listbox');
    eq(vp.getAttribute('aria-rowcount'), '50000');
    ok(rows(env).length > 0 && rows(env).length <= 40, 'rows ' + rows(env).length);
    var offsets = [0, 48 * 7, 48 * 12345, 48 * 25000, 48 * 49990, 48 * 50000, 48 * 3, 0];
    offsets.forEach(function (top) {
      scrollTo(env, top);
      env.clock.tick(200);
      var rs = rows(env);
      ok(rs.length <= 40, 'rows at ' + top + ': ' + rs.length);
      eq(W.VP_Debug.stats().cueRows, rs.length);
      var first = Number(rs[0].getAttribute('data-index'));
      for (var i = 0; i < rs.length; i++) {
        eq(Number(rs[i].getAttribute('data-index')), first + i, 'consecutive rows');
        eq(rs[i].getAttribute('aria-posinset'), String(first + i + 1));
        eq(rs[i].getAttribute('aria-setsize'), '50000');
        eq(rs[i].getAttribute('role'), 'option');
      }
      var visibleFirst = Math.min(Math.floor(top / 48), 50000 - 15);
      ok(first <= visibleFirst && first + rs.length >= Math.min(50000, visibleFirst + 15), 'window covers row ' + visibleFirst + ' (from ' + first + ')');
    });
    env.clock.tick(10000);
    eq(W.VP_Store.cueCount(), 50000, 'every cue loaded in the background');
    eq(L.stats().failedPages, 0);
    eq(W.VP_Store.get('cueCounts').loaded, 50000);
    ok(rows(env)[0].textContent.length > 3, 'loaded rows show text');
    ok(env.document.querySelector('.vp-cl-loading').hidden, 'loading line gone');
    eq(env.innerHTMLWrites, 0);
  });

  it('keeps the selection visible, the focus on the listbox and aria-activedescendant on the same cue after a re-render', function () {
    var env = setup();
    var W = env.window;
    var p = open(env, { path: 'C:\\x\\demo-2000-cues.vpoeta' });
    var L = mountList(env, p.cues);
    env.clock.tick(3000);
    var vp = viewport(env);
    vp.focus();
    ok(L.select(10));
    eq(vp.getAttribute('aria-activedescendant'), 'vp-cue-10');
    eq(env.document.querySelector('#vp-cue-10').getAttribute('aria-selected'), 'true');
    eq(W.VP_Store.get('selection').index, 10);
    L.select(1500);
    var top = vp.scrollTop;
    ok(1500 * 48 >= top && 1501 * 48 <= top + 640, 'selected row inside the viewport (scrollTop ' + top + ')');
    ok(env.document.querySelector('#vp-cue-1500'), 'row rendered');
    L.next();
    eq(L.selected(), 1501);
    L.prev();
    L.prev();
    eq(L.selected(), 1499);
    var c = W.VP_Store.getCue(1499);
    var changed = JSON.parse(JSON.stringify(c));
    changed.target = 'Salvē, amīce.';
    changed.state = 'edited';
    W.VP_Store.putCues([changed]);
    env.clock.tick(50);
    eq(env.document.activeElement, vp, 'focus stays on the listbox');
    eq(vp.getAttribute('aria-activedescendant'), 'vp-cue-1499', 'same cue after re-render');
    ok(env.document.querySelector('#vp-cue-1499 .vp-cue-text').textContent.indexOf('Salvē') === 0);
    ok(!env.document.querySelector('#vp-cue-1499 .vp-cue-lock').hasAttribute('hidden'), 'lock icon on an edited cue');
    scrollTo(env, 0);
    eq(env.document.querySelector('#vp-cue-1499'), null, 'scrolled away: row recycled');
    eq(vp.hasAttribute('aria-activedescendant'), false);
    L.select(1499);
    eq(vp.getAttribute('aria-activedescendant'), 'vp-cue-1499', 'selecting scrolls it back');
    env.document.querySelector('#vp-cue-1502').click();
    eq(L.selected(), 1502);
    eq(env.document.activeElement, vp);
  });

  it('filters, counts and macron-insensitive search; the selected cue stays listed until the selection moves', function () {
    var env = setup();
    var W = env.window;
    var p = open(env, { path: 'C:\\x\\Fabula.vpoeta' });
    var L = mountList(env, p.cues);
    var all = [];
    for (var i = 0; i < p.cues; i++) { all.push(W.VP_Store.getCue(i)); }
    function expect(pred) { return all.filter(pred).map(function (c) { return c.index; }); }
    var review = expect(function (c) { return c.state === 'translated' && c.confidence !== 'ok'; });
    ok(review.length > 0, 'the mock project has cues to review');
    L.setFilter('review');
    deepEq(L.viewIndices(), review);
    eq(L.counts().review, review.length);
    eq(env.document.querySelector('#vp-cl-filter').value, 'review');
    var opt = env.document.querySelectorAll('#vp-cl-filter option').filter(function (o) { return o.value === 'review'; })[0];
    ok(opt.textContent.indexOf('(' + review.length + ')') > 0, opt.textContent);
    L.setFilter('fix');
    deepEq(L.viewIndices(), expect(function (c) { return c.state === 'translated' && c.confidence === 'fix'; }));
    L.setFilter('names');
    deepEq(L.viewIndices(), expect(function (c) { return c.flags.indexOf('unknownName') >= 0; }));
    L.setFilter('emoji');
    deepEq(L.viewIndices(), expect(function (c) { return c.flags.indexOf('emoji') >= 0; }));
    L.setFilter('fast');
    deepEq(L.viewIndices(), expect(function (c) { return c.flags.indexOf('cps') >= 0 || c.cps > 17; }));
    L.setFilter('all');
    eq(L.viewIndices().length, 40);
    L.setQuery('insula');
    var insula = expect(function (c) { return /īnsulā/.test(c.target); });
    ok(insula.length > 0, 'some translated cue mentions the island');
    deepEq(L.viewIndices(), insula, 'no macrons typed, macrons in the text');
    L.setQuery('ISLAND');
    var island = expect(function (c) { return /island/.test(c.source); });
    ok(island.length > insula.length, 'untranslated cues match by their source');
    deepEq(L.viewIndices(), island, 'case-insensitive, source text too');
    L.setQuery('');
    eq(L.fold('Rēgīna PUELLAE; Ŏ ĭ ā ȳ'), 'regina puellae; o i a y');
    eq(L.fold('ἡ κόρη τὸ ῥόδον ὁρᾷ'), 'η κορη το ροδον ορα');
    eq(L.fold('¿Cómo estás, niño?'), '¿como estas, nino?');
    L.setQuery('zzzz');
    eq(L.viewIndices().length, 0);
    ok(!env.document.querySelector('.vp-cl-empty').hidden, 'empty state shown');
    env.document.querySelector('.vp-cl-empty button').click();
    eq(L.query(), '');
    eq(L.filter(), 'all');
    L.setFilter('review');
    var first = review[0];
    L.select(first);
    var c = JSON.parse(JSON.stringify(W.VP_Store.getCue(first)));
    c.state = 'reviewed';
    W.VP_Store.putCues([c]);
    env.clock.tick(50);
    ok(L.viewIndices().indexOf(first) >= 0, 'accepted cue still listed while selected');
    L.next();
    ok(L.viewIndices().indexOf(first) < 0, 'gone once the selection moved');
    eq(L.selected(), review[1]);
    var typed = env.document.querySelector('#vp-cl-search');
    typed.value = 'nauta';
    env.fire(typed, 'input');
    env.clock.tick(200);
    eq(L.query(), 'nauta', 'typing searches after a short pause');
    env.key(typed, 'Escape');
    eq(L.query(), '', 'Esc in the search box clears it');
  });

  it('nextReview walks the cues that need review; Accept all green reviews them with an Undo toast', function () {
    var env = setup();
    var W = env.window;
    var p = open(env, { path: 'C:\\x\\Fabula.vpoeta' });
    var L = mountList(env, p.cues);
    var first = L.firstReview();
    ok(first >= 0);
    L.select(0);
    eq(L.nextReview(1), first === 0 ? L.nextReview(1) : first);
    var green = L.counts().green;
    ok(green > 0, 'green cues exist');
    var btn = env.document.querySelector('.vp-cl-green');
    ok(btn.textContent.indexOf('(' + green + ')') > 0, btn.textContent);
    btn.click();
    env.clock.tick(100);
    eq(L.counts().green, 0);
    var reviewed = 0;
    for (var i = 0; i < p.cues; i++) { if (W.VP_Store.getCue(i).state === 'reviewed') { reviewed++; } }
    eq(reviewed, green);
    eq(W.VP_Toast.count(), 1);
    ok(env.document.querySelector('.vp-toast-text').textContent.indexOf('Accepted ' + green + ' green cues') === 0);
    eq(W.VP_History.size(), 1);
    env.document.querySelector('.vp-toast-action').click();
    env.clock.tick(200);
    eq(L.counts().green, green, 'undo brought them back');
    eq(W.VP_History.canRedo(), true);
  });

  it('destroy() returns listeners, timers and subscriptions to their baseline', function () {
    var env = setup();
    var W = env.window;
    var p = open(env, { path: 'C:\\x\\demo-3000-cues.vpoeta' });
    var before = W.VP_Router.counts();
    for (var i = 0; i < 20; i++) {
      W.VP_CueList.mount(env.box, { total: p.cues, kind: 'subs', pair: 'en-la' });
      env.clock.tick(30);
      scrollTo(env, 48 * 100 * i);
      W.VP_CueList.destroy();
      env.clock.tick(30);
    }
    deepEq(W.VP_Router.counts(), before);
    eq(env.box.childNodes.length, 0);
    eq(W.VP_CueList.isMounted(), false);
    deepEq(W.VP_I18n.missing(), []);
  });
});
