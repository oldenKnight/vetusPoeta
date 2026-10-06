describe('VP_I18n', function () {
  var EN = {
    'a.greet.label': 'Hello {name}',
    'a.count.one': '{n} cue',
    'a.count.other': '{n} cues',
    'a.count.zero': 'No cues',
    'a.only.label': 'English only',
    'a.title.tooltip': 'Tip',
    'a.ph.placeholder': 'Type',
    'a.aria.aria': 'Close'
  };
  var ES = {
    'a.greet.label': 'Hola {name}',
    'a.count.one': '{n} subtítulo',
    'a.count.other': '{n} subtítulos',
    'a.count.zero': 'Sin subtítulos',
    'a.title.tooltip': 'Consejo',
    'a.ph.placeholder': 'Escribe',
    'a.aria.aria': 'Cerrar'
  };
  function setup() {
    var env = load(['vp_dom.js', 'vp_i18n.js']);
    env.window.VP_I18n.load('en-US', EN);
    env.window.VP_I18n.load('es-MX', ES);
    return env;
  }

  it('fills placeholders, formats numbers, keeps unknown placeholders', function () {
    var I = setup().window.VP_I18n;
    eq(I.t('a.greet.label', { name: 'Ana' }), 'Hello Ana');
    eq(I.t('a.greet.label'), 'Hello {name}');
    eq(I.t('a.greet.label', { name: 12345 }), 'Hello 12,345');
  });

  it('picks .zero/.one/.other from vars.n', function () {
    var I = setup().window.VP_I18n;
    eq(I.t('a.count', { n: 0 }), 'No cues');
    eq(I.t('a.count', { n: 1 }), '1 cue');
    eq(I.t('a.count', { n: 2 }), '2 cues');
    eq(I.t('a.count', { n: 1019 }), '1,019 cues');
    I.setLang('es-MX');
    eq(I.t('a.count', { n: 1 }), '1 subtítulo');
    eq(I.t('a.count', { n: 3 }), '3 subtítulos');
  });

  it('falls back to en-US, then to the key, and records missing keys', function () {
    var I = setup().window.VP_I18n;
    I.setLang('es-MX');
    eq(I.t('a.only.label'), 'English only');
    eq(I.t('no.such.key'), 'no.such.key');
    deepEq(I.missing(), ['no.such.key']);
    ok(I.has('a.only.label'));
    eq(I.has('nope'), false);
  });

  it('num() uses comma thousands and period decimals in both languages', function () {
    var I = setup().window.VP_I18n;
    eq(I.num(1019), '1,019');
    eq(I.num(1234567.5), '1,234,567.5');
    eq(I.num(3.14159, 2), '3.14');
    eq(I.num(-2500), '-2,500');
    eq(I.num(0.25), '0.25');
    I.setLang('es-MX');
    eq(I.num(1019), '1,019');
    eq(I.num(12.5), '12.5');
  });

  it('bind() fills text, title, placeholder and aria-label, with data-i18n-vars', function () {
    var env = setup();
    var D = env.window.VP_Dom;
    var root = D.el('div', { 'data-i18n-title': 'a.title.tooltip' }, [
      D.el('span', { id: 's', 'data-i18n': 'a.greet.label', 'data-i18n-vars': { name: 'Marco' } }),
      D.el('input', { id: 'i', 'data-i18n-placeholder': 'a.ph.placeholder' }),
      D.el('button', { id: 'b', 'data-i18n-aria': 'a.aria.aria' })
    ]);
    env.window.VP_I18n.bind(root);
    eq(root.getAttribute('title'), 'Tip');
    eq(root.querySelector('#s').textContent, 'Hello Marco');
    eq(root.querySelector('#i').getAttribute('placeholder'), 'Type');
    eq(root.querySelector('#b').getAttribute('aria-label'), 'Close');
  });

  it('setLang() switches <html lang>, re-binds the page and notifies listeners until removed', function () {
    var env = setup();
    var I = env.window.VP_I18n;
    var span = env.window.VP_Dom.el('span', { 'data-i18n': 'a.greet.label', 'data-i18n-vars': { name: 'Ana' } });
    env.document.body.appendChild(span);
    I.bind(span);
    var calls = [];
    var remove = I.onLanguageChanged(function (l) { calls.push(l); });
    eq(I.listenerCount(), 1);
    ok(I.setLang('es-MX'));
    eq(env.document.documentElement.getAttribute('lang'), 'es-MX');
    eq(span.textContent, 'Hola Ana');
    remove();
    eq(I.listenerCount(), 0);
    I.setLang('en-US');
    deepEq(calls, ['es-MX']);
    eq(I.setLang('fr-FR'), false);
    eq(I.lang(), 'en-US');
  });

  it('the real tables: every key translates in both languages and keeps its placeholders', function () {
    var env = load(['vp_dom.js', 'vp_i18n.js']);
    var I = env.window.VP_I18n;
    var en = readJson('i18n/en-US.json');
    var es = readJson('i18n/es-MX.json');
    I.load('en-US', en);
    I.load('es-MX', es);
    I.setLang('es-MX');
    Object.keys(en).forEach(function (k) {
      var vars = { n: 2, name: 'N', version: 'V', lang: 'L', total: 6 };
      var text = I.t(k, vars);
      ok(text !== k, k + ' resolves');
      ok(text.indexOf('{') < 0, k + ' has no unfilled placeholder: ' + text);
    });
    deepEq(I.missing(), []);
  });

  it('the xx-LONG pseudo-locale (+40 % text) loads, keeps placeholders and binds', function () {
    var env = load(['vp_dom.js', 'vp_i18n.js']);
    var I = env.window.VP_I18n;
    var en = readJson('i18n/en-US.json');
    var longDict = pseudoLocale(en);
    I.load('en-US', en);
    I.load('xx-LONG', longDict);
    ok(I.setLang('xx-LONG'));
    var a = 0;
    var b = 0;
    Object.keys(en).forEach(function (k) {
      a += en[k].length;
      b += longDict[k].length;
      var ph = (en[k].match(/\{\w+\}/g) || []).join(',');
      eq((longDict[k].match(/\{\w+\}/g) || []).join(','), ph, k + ' placeholders');
    });
    ok(b / a >= 1.3, 'pseudo-locale is at least 30 % longer (' + (b / a).toFixed(2) + ')');
    eq(I.t('tour.counter.label', { n: 2, total: 6 }), '[Steep 2 oof 6]');
    var el = env.window.VP_Dom.el('span', { 'data-i18n': 'dialog.ok.cta' });
    I.bind(el);
    eq(el.textContent, '[OOK]');
  });
});
