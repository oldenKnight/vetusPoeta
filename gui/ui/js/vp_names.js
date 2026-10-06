/* vp_names.js - the Names tab: glossary of proper names (DESIGN 13; PREDESIGN 1.2, 4.2).
 *
 * names.list gives the detected names with their policy; per name: occurrences, policy
 * radio Keep | Decline | Translate, Latin form, gender and declension selects, Apply ->
 * names.set, which answers the affected cues (the engine marks them stale); the toast says
 * how many and offers Undo (the previous policy is sent again). "Copy as CSV" puts the
 * glossary on the clipboard (name, policy, form, gender, declension, count): the engine has
 * no names.export/import command, so the UI offers the clipboard instead of a file.
 *
 * B8: the real names.list has no detection, it lists the names set through names.set (and
 * no count, so the count is shown only when the engine sends one). The detected-names list
 * appears only when the engine returns names; the "Add a name" form (name, what to do with
 * it, Latin form) is always there and sends names.set.
 *
 * VP_Names.mount(el) / destroy(); reload() -> Promise; apply(name) -> Promise; csv(names)
 * (pure); copyCsv() -> Promise(bool); names(); add({name, policy, form}) -> Promise; stats()
 */
(function () {
  'use strict';

  var OWNER = 'names';
  var POLICIES = ['keep', 'decline', 'translate'];
  var GENDERS = ['', 'masculine', 'feminine', 'neuter'];
  var DECLENSIONS = ['', '1', '2', '3', '4', '5'];
  var s = null;

  function T(key, vars) { return window.VP_I18n.t(key, vars); }
  function el(tag, attrs, kids) { return window.VP_Dom.el(tag, attrs, kids); }
  function P() { return window.Promise; }

  function i18nEl(tag, cls, key, vars, extra) {
    var a = { className: cls, 'data-i18n': key, 'data-i18n-vars': vars || null, text: T(key, vars) };
    if (extra) { for (var k in extra) { if (Object.prototype.hasOwnProperty.call(extra, k)) { a[k] = extra[k]; } } }
    return el(tag, a);
  }

  function showError(err) {
    if (window.VP_App && typeof window.VP_App.showError === 'function') { window.VP_App.showError(err); }
  }

  function csvCell(v) {
    var x = String(v === undefined || v === null ? '' : v);
    return /[",\r\n]/.test(x) ? '"' + x.replace(/"/g, '""') + '"' : x;
  }

  function csv(names) {
    var rows = [['name', 'policy', 'form', 'gender', 'declension', 'count']];
    (names || []).forEach(function (n) { rows.push([n.name, n.policy, n.form, n.gender, n.declension, n.count]); });
    return rows.map(function (r) { return r.map(csvCell).join(','); }).join('\r\n') + '\r\n';
  }

  // Clipboard: the async API when the page has it, else a hidden textarea + execCommand.
  function copyText(text) {
    var nav = window.navigator;
    if (nav && nav.clipboard && typeof nav.clipboard.writeText === 'function') {
      return nav.clipboard.writeText(text).then(function () { return true; }, function () { return copyFallback(text); });
    }
    return P().resolve(copyFallback(text));
  }

  function copyFallback(text) {
    var ta = el('textarea', { className: 'vp-visually-hidden', 'aria-hidden': 'true' });
    ta.value = text;
    document.body.appendChild(ta);
    var ok = false;
    try {
      ta.focus();
      if (typeof ta.select === 'function') { ta.select(); }
      ok = !!(document.execCommand && document.execCommand('copy'));
    } catch (e) {
      ok = false;
    }
    document.body.removeChild(ta);
    return ok;
  }

  function slug(name) { return String(name).replace(/[^A-Za-z0-9]/g, function (c) { return '_' + c.charCodeAt(0).toString(16); }); }

  function render() {
    if (!s) { return; }
    var D = window.VP_Dom;
    D.clear(s.list);
    if (!s.loaded) {
      s.list.appendChild(i18nEl('p', 'vp-hint', 'names.loading.label'));
      return;
    }
    s.list.hidden = !s.names.length;
    s.emptyEl.hidden = !!s.names.length;
    if (!s.names.length) { return; }
    s.names.forEach(function (n) {
      var id = 'vp-name-' + slug(n.name);
      var policy = POLICIES.indexOf(n.policy) >= 0 ? n.policy : 'keep';
      s.list.appendChild(el('li', { className: 'vp-name', dataset: { name: n.name } }, [
        el('div', { className: 'vp-name-head' }, [
          el('span', { className: 'vp-name-text vp-text', text: n.name }),
          typeof n.count === 'number' ? el('span', { className: 'vp-name-count', text: T('names.count', { n: n.count }) }) : null
        ]),
        el('fieldset', { className: 'vp-name-policy' }, [
          i18nEl('legend', 'vp-visually-hidden', 'names.policy.label', { name: n.name })
        ].concat(POLICIES.map(function (pol) {
          return el('label', { className: 'vp-radio' }, [
            el('input', { type: 'radio', name: id + '-policy', value: pol, checked: pol === policy, dataset: { nameField: 'policy' } }),
            i18nEl('span', null, 'names.policy.' + pol + '.label')
          ]);
        }))),
        el('div', { className: 'vp-name-fields' }, [
          el('div', { className: 'vp-field' }, [
            el('label', { htmlFor: id + '-form', 'data-i18n': 'names.form.label', text: T('names.form.label') }),
            el('input', { id: id + '-form', type: 'text', className: 'vp-input vp-text', lang: 'la', value: n.form || '', spellcheck: 'false', dataset: { nameField: 'form' } })
          ]),
          el('div', { className: 'vp-field' }, [
            el('label', { htmlFor: id + '-gender', 'data-i18n': 'names.gender.label', text: T('names.gender.label') }),
            el('select', { id: id + '-gender', className: 'vp-input', dataset: { nameField: 'gender' } }, GENDERS.map(function (g) {
              return el('option', { value: g, selected: String(n.gender || '') === g || (g && String(n.gender || '').charAt(0) === g.charAt(0)), 'data-i18n': g ? 'grammar.gender.' + g + '.label' : 'names.gender.unknown.label', text: g ? T('grammar.gender.' + g + '.label') : T('names.gender.unknown.label') });
            }))
          ]),
          el('div', { className: 'vp-field' }, [
            el('label', { htmlFor: id + '-decl', 'data-i18n': 'names.declension.label', text: T('names.declension.label') }),
            el('select', { id: id + '-decl', className: 'vp-input', dataset: { nameField: 'declension' } }, DECLENSIONS.map(function (d) {
              return el('option', { value: d, selected: String(n.declension || '') === d, 'data-i18n': d ? 'names.declension.' + d + '.label' : 'names.declension.unknown.label', text: d ? T('names.declension.' + d + '.label') : T('names.declension.unknown.label') });
            }))
          ])
        ]),
        el('div', { className: 'vp-row' }, [
          i18nEl('button', 'vp-btn vp-btn-secondary', 'names.apply.cta', null, { type: 'button', dataset: { namesAction: 'apply' } })
        ])
      ]));
      // Selects take their value once the options are in place (the selected flag alone is not
      // enough for every browser when the option is built before it is attached).
      var gsel = D.qs('#' + id + '-gender', s.list);
      var dsel = D.qs('#' + id + '-decl', s.list);
      if (gsel) { gsel.value = GENDERS.indexOf(String(n.gender || '')) >= 0 ? String(n.gender || '') : (String(n.gender || '').charAt(0) === 'm' ? 'masculine' : (String(n.gender || '').charAt(0) === 'f' ? 'feminine' : (String(n.gender || '').charAt(0) === 'n' ? 'neuter' : ''))); }
      if (dsel) { dsel.value = DECLENSIONS.indexOf(String(n.declension || '')) >= 0 ? String(n.declension || '') : ''; }
    });
  }

  function reload() {
    if (!s) { return P().resolve([]); }
    var gen = s.gen;
    return window.VP_Bridge.call('names.list', {}).then(function (r) {
      if (!s || s.gen !== gen) { return []; }
      s.names = (r && r.names) || [];
      s.loaded = true;
      render();
      return s.names;
    }, function (err) {
      if (!s || s.gen !== gen) { return []; }
      s.names = [];
      s.loaded = true;
      render();
      showError(err);
      return [];
    });
  }

  function find(name) {
    for (var i = 0; i < s.names.length; i++) { if (s.names[i].name === name) { return s.names[i]; } }
    return null;
  }

  function readRow(li) {
    var D = window.VP_Dom;
    var out = {};
    D.qsa('[data-name-field]', li).forEach(function (f) {
      var k = f.getAttribute('data-name-field');
      if (k === 'policy') {
        if (f.checked) { out.policy = f.value; }
      } else {
        out[k] = f.value;
      }
    });
    return out;
  }

  function apply(name) {
    if (!s) { return P().resolve(null); }
    var li = window.VP_Dom.qs('[data-name="' + name.replace(/"/g, '\\"') + '"]', s.list);
    var prev = find(name);
    if (!li || !prev) { return P().resolve(null); }
    var v = readRow(li);
    var params = { name: name, policy: v.policy || prev.policy || 'keep', form: v.form || '', gender: v.gender || '', declension: v.declension || '' };
    var before = { name: name, policy: prev.policy || 'keep', form: prev.form || '', gender: prev.gender || '', declension: prev.declension || '' };
    s.busy = true;
    return window.VP_Bridge.call('names.set', params).then(function (r) {
      var affected = (r && r.affectedCues) || [];
      var W = window.VP_Workspace;
      var after = affected.length && W && W.isMounted() ? W.cmd.refetch(affected) : P().resolve(0);
      return after.then(function () {
        window.VP_Toast.undoable('names.applied.label', { n: affected.length }, function () {
          window.VP_Bridge.call('names.set', before).then(function (r2) {
            var again = (r2 && r2.affectedCues) || [];
            if (again.length && W && W.isMounted()) { W.cmd.refetch(again); }
            reload();
          }, showError);
        });
        return reload().then(function () { return affected; });
      });
    }).then(function (x) {
      if (s) { s.busy = false; }
      return x;
    }, function (err) {
      if (s) { s.busy = false; }
      showError(err);
      return null;
    });
  }

  // "Add a name": names.set with the typed name; the engine marks the cues that contain it.
  function add(v) {
    if (!s) { return P().resolve(null); }
    v = v || { name: s.addName.value, policy: s.addPolicy.value, form: s.addForm.value };
    var name = String(v.name || '').replace(/^\s+|\s+$/g, '');
    if (!name) {
      s.addError.hidden = false;
      s.addName.setAttribute('aria-invalid', 'true');
      s.addName.focus();
      return P().resolve(null);
    }
    s.addError.hidden = true;
    s.addName.removeAttribute('aria-invalid');
    var params = { name: name, policy: POLICIES.indexOf(v.policy) >= 0 ? v.policy : 'decline' };
    var form = String(v.form || '').replace(/^\s+|\s+$/g, '');
    if (form) { params.form = form; }
    return window.VP_Bridge.call('names.set', params).then(function (r) {
      var affected = (r && r.affectedCues) || [];
      var W = window.VP_Workspace;
      if (affected.length && W && W.isMounted()) { W.cmd.refetch(affected); }
      if (s) {
        s.addName.value = '';
        s.addForm.value = '';
      }
      window.VP_Toast.show({ key: 'names.add.done.label', vars: { name: name, n: affected.length }, kind: 'success' });
      return reload().then(function () { return affected; });
    }, function (err) {
      showError(err);
      return null;
    });
  }

  function copyCsv() {
    var text = csv(s ? s.names : []);
    return copyText(text).then(function (ok) {
      window.VP_Toast.show({ key: ok ? 'names.csv.done.label' : 'names.csv.fail.label', kind: ok ? 'success' : 'error' });
      return ok;
    });
  }

  function onClick(e, btn) {
    var a = btn.getAttribute('data-names-action');
    if (a === 'apply') {
      var li = window.VP_Dom.closest(btn, '.vp-name', s.root);
      if (li) { apply(li.getAttribute('data-name')); }
    } else if (a === 'csv') {
      copyCsv();
    } else if (a === 'reload') {
      reload();
    } else if (a === 'add') {
      add();
    }
  }

  function mount(root) {
    if (s) { destroy(); }
    s = { gen: (mount.gen = (mount.gen || 0) + 1), names: [], loaded: false, busy: false, removers: [] };
    s.list = el('ul', { className: 'vp-name-list' });
    s.emptyEl = i18nEl('p', 'vp-hint vp-name-empty', 'names.panel.empty', null, { hidden: true });
    s.addName = el('input', { id: 'vp-name-add-name', type: 'text', className: 'vp-input', spellcheck: 'false', 'aria-describedby': 'vp-name-add-error' });
    s.addForm = el('input', { id: 'vp-name-add-form', type: 'text', className: 'vp-input vp-text', lang: 'la', spellcheck: 'false' });
    s.addPolicy = el('select', { id: 'vp-name-add-policy', className: 'vp-input' }, POLICIES.map(function (pol) {
      return el('option', { value: pol, selected: pol === 'decline', 'data-i18n': 'names.policy.' + pol + '.label', text: T('names.policy.' + pol + '.label') });
    }));
    s.addError = i18nEl('p', 'vp-field-error', 'names.add.empty.label', null, { id: 'vp-name-add-error', role: 'alert', hidden: true });
    s.addEl = el('section', { className: 'vp-name-add', 'aria-labelledby': 'vp-name-add-title' }, [
      i18nEl('h3', null, 'names.add.title', null, { id: 'vp-name-add-title' }),
      el('div', { className: 'vp-name-fields' }, [
        el('div', { className: 'vp-field' }, [el('label', { htmlFor: 'vp-name-add-name', 'data-i18n': 'names.add.name.label', text: T('names.add.name.label') }), s.addName]),
        el('div', { className: 'vp-field' }, [el('label', { htmlFor: 'vp-name-add-policy', 'data-i18n': 'names.add.policy.label', text: T('names.add.policy.label') }), s.addPolicy]),
        el('div', { className: 'vp-field' }, [el('label', { htmlFor: 'vp-name-add-form', 'data-i18n': 'names.form.label', text: T('names.form.label') }), s.addForm])
      ]),
      s.addError,
      el('div', { className: 'vp-row' }, [i18nEl('button', 'vp-btn vp-btn-secondary', 'names.add.cta', null, { type: 'button', dataset: { namesAction: 'add' } })])
    ]);
    s.root = el('div', { className: 'vp-panel vp-panel-names' }, [
      i18nEl('h2', 'vp-panel-title', 'names.panel.title'),
      i18nEl('p', 'vp-hint', 'names.panel.hint'),
      s.list,
      s.emptyEl,
      s.addEl,
      el('div', { className: 'vp-row' }, [
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'names.csv.cta', null, { type: 'button', dataset: { namesAction: 'csv' } }),
        i18nEl('button', 'vp-btn vp-btn-tertiary', 'names.reload.cta', null, { type: 'button', dataset: { namesAction: 'reload' } })
      ]),
      i18nEl('p', 'vp-hint', 'names.csv.hint')
    ]);
    root.appendChild(s.root);
    window.VP_Dom.delegate(s.root, '[data-names-action]', 'click', onClick, { owner: OWNER });
    window.VP_Dom.on(s.addName, 'keydown', function (e) {
      if (e.key === 'Enter') {
        e.preventDefault();
        add();
      }
    }, { owner: OWNER });
    s.removers.push(window.VP_I18n.onLanguageChanged(render));
    render();
    reload();
  }

  function destroy() {
    if (!s) { return; }
    window.VP_Dom.offOwner(OWNER);
    window.VP_Timers.clearAll(OWNER);
    while (s.removers.length) { s.removers.pop()(); }
    if (s.root && s.root.parentNode) { s.root.parentNode.removeChild(s.root); }
    s = null;
  }

  function i18nKeys() {
    var keys = ['names.count', 'names.applied.label', 'names.policy.label', 'names.gender.unknown.label', 'names.declension.unknown.label', 'names.csv.done.label', 'names.csv.fail.label', 'names.add.done.label'];
    POLICIES.forEach(function (p) { keys.push('names.policy.' + p + '.label'); });
    DECLENSIONS.forEach(function (d) { if (d) { keys.push('names.declension.' + d + '.label'); } });
    return keys;
  }

  window.VP_Names = {
    mount: mount,
    destroy: destroy,
    isMounted: function () { return s !== null; },
    reload: reload,
    apply: apply,
    csv: csv,
    copyCsv: copyCsv,
    copyText: copyText,
    add: add,
    names: function () { return s ? s.names.slice() : []; },
    stats: function () { return s ? { names: s.names.length, loaded: s.loaded } : null; },
    i18nKeys: i18nKeys
  };
}());
