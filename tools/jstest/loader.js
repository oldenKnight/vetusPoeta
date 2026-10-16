/* loader.js - runs UI files in a fresh vm context over the DOM stub (PREPLAN 4.4 items 1-2).
 *
 * TABLE is DESIGN section 13: file -> global, in load order (index.html must follow it).
 * load(files, opts) -> env (domstub env + g: the context global, run(code))
 * contract(file)    -> {errors:[...], global}
 */
'use strict';

var fs = require('fs');
var path = require('path');
var vm = require('vm');
var domstub = require('./domstub');

var UI_ROOT = path.resolve(__dirname, '..', '..', 'gui', 'ui');
var JS_DIR = path.join(UI_ROOT, 'js');

var TABLE = [
  ['polyfill_promise.js', null],
  ['vp_dom.js', 'VP_Dom'],
  ['vp_timers.js', 'VP_Timers'],
  ['vp_i18n.js', 'VP_I18n'],
  ['vp_bridge.js', 'VP_Bridge'],
  ['vp_mock_engine.js', 'VP_MockEngine'],
  ['vp_store.js', 'VP_Store'],
  ['vp_history.js', 'VP_History'],
  ['vp_keys.js', 'VP_Keys'],
  ['vp_toast.js', 'VP_Toast'],
  ['vp_dialog.js', 'VP_Dialog'],
  ['vp_tour.js', 'VP_Tour'],
  ['vp_router.js', 'VP_Router'],
  ['vp_start.js', 'VP_Start'],
  ['vp_workspace.js', 'VP_Workspace'],
  ['vp_cuelist.js', 'VP_CueList'],
  ['vp_panes.js', 'VP_Panes'],
  ['vp_inspector.js', 'VP_Inspector'],
  ['vp_engines.js', 'VP_Engines'],
  ['vp_names.js', 'VP_Names'],
  ['vp_corrections.js', 'VP_Corrections'],
  ['vp_words.js', 'VP_Words'],
  ['vp_orberg.js', 'VP_Orberg'],
  ['vp_export.js', 'VP_Export'],
  ['vp_settings.js', 'VP_Settings'],
  ['vp_about.js', 'VP_About'],
  ['vp_debug.js', 'VP_Debug'],
  ['vp_app.js', 'VP_App']
];

function tableIndex(file) {
  for (var i = 0; i < TABLE.length; i++) { if (TABLE[i][0] === file) { return i; } }
  return -1;
}

function presentFiles(jsDir) {
  jsDir = jsDir || JS_DIR;
  return TABLE.map(function (r) { return r[0]; }).filter(function (f) { return fs.existsSync(path.join(jsDir, f)); });
}

function globalNames(ctx) {
  return vm.runInContext('Object.getOwnPropertyNames(this)', ctx).slice();
}

function makeContext(opts) {
  var env = domstub.createEnv(opts);
  var ctx = vm.createContext(env.window);
  if (!opts || !opts.keepPromise) { vm.runInContext('delete this.Promise;', ctx); }
  env.ctx = ctx;
  env.g = env.window;
  env.run = function (code, filename) { return vm.runInContext(code, ctx, { filename: filename || 'inline.js' }); };
  return env;
}

function runFile(env, file, jsDir) {
  var full = path.isAbsolute(file) ? file : path.join(jsDir || JS_DIR, file);
  var src = fs.readFileSync(full, 'utf8');
  vm.runInContext(src, env.ctx, { filename: full });
}

// files: array of base names (table order is not enforced here) or 'all' (every present file).
// opts: domstub options plus skip (file names to leave out), noPolyfill (do not prepend polyfill_promise.js) and keepPromise
// (leave the context's native Promise in place).
function load(files, opts) {
  opts = opts || {};
  if (opts.root === undefined) { opts.root = UI_ROOT; }
  var env = makeContext(opts);
  var list = files === 'all' ? presentFiles(opts.jsDir) : files.slice();
  if (opts.skip) { list = list.filter(function (f) { return opts.skip.indexOf(f) < 0; }); }
  if (!opts.noPolyfill && list.indexOf('polyfill_promise.js') < 0) { list.unshift('polyfill_promise.js'); }
  list.forEach(function (f) { runFile(env, f, opts.jsDir); });
  return env;
}

// One file alone in a fresh context: exactly one new global with the right name, no leaks,
// no listeners or timers registered at load time, IIFE wrapped (checked by es5lint.checkIIFE).
function contract(file, opts) {
  opts = opts || {};
  var jsDir = opts.jsDir || JS_DIR;
  var base = path.basename(file);
  var errors = [];
  var idx = tableIndex(base);
  var expected = idx >= 0 ? TABLE[idx][1] : undefined;
  if (idx < 0 && !opts.anyName) { errors.push('not listed in DESIGN section 13 (tools/jstest/loader.js TABLE)'); }
  var env = makeContext({ root: UI_ROOT });
  var before = globalNames(env.ctx);
  try {
    runFile(env, path.isAbsolute(file) ? file : path.join(jsDir, file));
  } catch (e) {
    errors.push('throws while loading: ' + (e && e.message));
    return { errors: errors, global: null };
  }
  var after = globalNames(env.ctx);
  var added = after.filter(function (n) { return before.indexOf(n) < 0; });
  var removed = before.filter(function (n) { return after.indexOf(n) < 0; });
  if (removed.length) { errors.push('removes globals: ' + removed.join(', ')); }
  var name = null;
  if (expected === null) {
    if (added.length !== 1 || added[0] !== 'Promise') { errors.push('polyfill must add exactly "Promise" when it is missing; added: [' + added.join(', ') + ']'); }
  } else {
    var vpNames = added.filter(function (n) { return /^VP_[A-Za-z0-9]+$/.test(n); });
    var leaks = added.filter(function (n) { return !/^VP_[A-Za-z0-9]+$/.test(n); });
    if (vpNames.length !== 1) { errors.push('must define exactly one VP_<Name> global; found [' + vpNames.join(', ') + ']'); }
    if (leaks.length) { errors.push('leaks globals: ' + leaks.join(', ')); }
    name = vpNames[0] || null;
    if (expected && name && name !== expected) { errors.push('defines ' + name + ' but DESIGN section 13 says ' + expected); }
  }
  if (env.listenerCount() !== 0) { errors.push('adds ' + env.listenerCount() + ' event listener(s) at load time'); }
  if (env.clock.pending() !== 0) { errors.push('starts ' + env.clock.pending() + ' timer(s) at load time'); }
  return { errors: errors, global: name };
}

module.exports = {
  TABLE: TABLE, UI_ROOT: UI_ROOT, JS_DIR: JS_DIR,
  load: load, contract: contract, presentFiles: presentFiles, tableIndex: tableIndex
};
