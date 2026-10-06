/* engine_bridge_shim.js - DEV ONLY: runs the UI in a browser against the real engine.
 *   node gui/ui/dev/engine_bridge_shim.js --engine build/engine/cli/vpengine [--lexicons data/work]
 *        [--data <dir>] [--port 8124] [--open-file <path>] [--save-dir <dir>]
 * Serves gui/ui over http://127.0.0.1 (like serve.js) and starts `vpengine serve` as a child process. index.html
 * gets one extra <script src="/__vp/bridge.js"> that defines window.chrome.webview the way the Windows shell does:
 * postMessage(text) goes to the engine's stdin (POST /__vp/post), every engine stdout line comes back as a
 * 'message' event (Server-Sent Events from /__vp/events). The shell-handled commands (dialog.openFile,
 * dialog.saveFile, shell.revealFile, shell.openExternal) are answered here: openFile returns --open-file (default
 * the English sample), saveFile a path in --save-dir. Used by smoke_real.js; not part of the app bundle.
 */
'use strict';

var http = require('http');
var fs = require('fs');
var os = require('os');
var path = require('path');
var childProcess = require('child_process');

var UI = path.resolve(__dirname, '..');
var REPO = path.resolve(UI, '..', '..');
var TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8', '.ttf': 'font/ttf', '.svg': 'image/svg+xml', '.txt': 'text/plain; charset=utf-8',
  '.png': 'image/png', '.ico': 'image/x-icon'
};
var SHELL_CMDS = { 'dialog.openFile': 1, 'dialog.saveFile': 1, 'shell.revealFile': 1, 'shell.openExternal': 1 };

// Browser side (ES5, same origin, allowed by the page's CSP default-src 'self').
var BRIDGE_JS = [
  '(function () {',
  '  "use strict";',
  '  var listeners = [];',
  '  var es = new EventSource("/__vp/events");',
  '  es.onmessage = function (e) {',
  '    var list = listeners.slice();',
  '    for (var i = 0; i < list.length; i++) { try { list[i]({ data: e.data }); } catch (err) { if (window.console) { window.console.error(err); } } }',
  '  };',
  '  window.chrome = window.chrome || {};',
  '  window.chrome.webview = {',
  '    addEventListener: function (type, fn) { if (type === "message" && listeners.indexOf(fn) < 0) { listeners.push(fn); } },',
  '    removeEventListener: function (type, fn) { var i = listeners.indexOf(fn); if (i >= 0) { listeners.splice(i, 1); } },',
  '    postMessage: function (text) {',
  '      var x = new XMLHttpRequest();',
  '      x.open("POST", "/__vp/post", true);',
  '      x.setRequestHeader("Content-Type", "text/plain; charset=utf-8");',
  '      x.send(typeof text === "string" ? text : JSON.stringify(text));',
  '    }',
  '  };',
  '}());',
  ''
].join('\n');

function start(opts, cb) {
  opts = opts || {};
  var engineExe = path.resolve(opts.engine || path.join(REPO, 'build', 'engine', 'cli', 'vpengine'));
  var dataDir = opts.data || fs.mkdtempSync(path.join(os.tmpdir(), 'vp-real-'));
  var lexicons = path.resolve(opts.lexicons || path.join(REPO, 'data', 'work'));
  var saveDir = opts.saveDir || dataDir;
  var openFile = opts.openFile || path.join(dataDir, 'samples', 'sample.en.srt');
  var env = Object.assign({}, process.env, opts.env || {});
  if (!env.VP_SAMPLES_DIR) { env.VP_SAMPLES_DIR = path.join(REPO, 'tests', 'samples'); }
  var logPath = path.join(dataDir, 'engine.log');
  var log = fs.openSync(logPath, 'a');
  var child = childProcess.spawn(engineExe, ['serve', '--data', dataDir, '--lexicons', lexicons].concat(opts.engineArgs || []),
    { stdio: ['pipe', 'pipe', log], env: env });
  var clients = [];
  var backlog = [];
  var shellCalls = [];
  var exited = null;
  var partial = '';

  function deliver(line) {
    if (!clients.length) {
      if (backlog.length < 100000) { backlog.push(line); }
      return;
    }
    for (var i = 0; i < clients.length; i++) { clients[i].write('data: ' + line + '\n\n'); }
  }
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', function (chunk) {
    partial += chunk;
    var nl;
    while ((nl = partial.indexOf('\n')) >= 0) {
      var line = partial.slice(0, nl).replace(/\r$/, '');
      partial = partial.slice(nl + 1);
      if (line) { deliver(line); }
    }
  });
  child.on('exit', function (code, signal) { exited = { code: code, signal: signal }; });
  child.on('error', function (e) { exited = { error: String(e && e.message) }; });

  function shell(msg) {
    var p = msg.params || {};
    var result = {};
    shellCalls.push({ cmd: msg.cmd, params: p });
    if (msg.cmd === 'dialog.openFile') {
      result = { path: openFile, paths: [openFile], cancelled: false };
    } else if (msg.cmd === 'dialog.saveFile') {
      var file = path.join(saveDir, String(p.suggestedName || 'export.srt').replace(/[\\/]/g, '_'));
      result = { path: file, cancelled: false };
    }
    deliver(JSON.stringify({ id: msg.id, ok: true, result: result }));
  }

  var server = http.createServer(function (req, res) {
    var url = String(req.url).split('?')[0];
    if (url === '/__vp/bridge.js') {
      res.writeHead(200, { 'Content-Type': TYPES['.js'], 'Cache-Control': 'no-store' });
      res.end(BRIDGE_JS);
      return;
    }
    if (url === '/__vp/events') {
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
      res.write(': connected\n\n');
      clients.push(res);
      var queued = backlog;
      backlog = [];
      for (var q = 0; q < queued.length; q++) { res.write('data: ' + queued[q] + '\n\n'); }
      req.on('close', function () { var i = clients.indexOf(res); if (i >= 0) { clients.splice(i, 1); } });
      return;
    }
    if (url === '/__vp/post' && req.method === 'POST') {
      var body = '';
      req.setEncoding('utf8');
      req.on('data', function (c) { body += c; });
      req.on('end', function () {
        res.writeHead(204);
        res.end();
        var msg = null;
        try { msg = JSON.parse(body); } catch (e) { msg = null; }
        if (msg && SHELL_CMDS[msg.cmd]) {
          shell(msg);
        } else if (!exited) {
          child.stdin.write(body.replace(/\n/g, ' ') + '\n');
        }
      });
      return;
    }
    var rel;
    try { rel = decodeURIComponent(url); } catch (e) {
      res.writeHead(400);
      res.end();
      return;
    }
    if (rel === '/') { rel = '/index.html'; }
    var file = path.normalize(path.join(UI, rel));
    if (file.indexOf(UI + path.sep) !== 0) {
      res.writeHead(403);
      res.end();
      return;
    }
    fs.readFile(file, function (err, data) {
      if (err) {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('not found');
        return;
      }
      if (rel === '/index.html') {
        data = Buffer.from(String(data).replace('<script src="js/polyfill_promise.js"></script>',
          '<script src="/__vp/bridge.js"></script>\n<script src="js/polyfill_promise.js"></script>'));
      }
      res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
      res.end(data);
    });
  });
  server.on('error', function (e) { cb(e); });
  server.listen(opts.port || 0, '127.0.0.1', function () {
    var base = 'http://127.0.0.1:' + server.address().port + '/';
    cb(null, {
      url: base,
      dataDir: dataDir,
      logPath: logPath,
      shellCalls: function () { return shellCalls.slice(); },
      exited: function () { return exited; },
      close: function (done) {
        for (var i = 0; i < clients.length; i++) { clients[i].end(); }
        clients = [];
        var finished = false;
        function finish() {
          if (finished) { return; }
          finished = true;
          try { fs.closeSync(log); } catch (e) { /* closed */ }
          server.close(function () { if (done) { done(exited); } });
        }
        if (exited) {
          finish();
          return;
        }
        child.on('exit', finish);
        try { child.stdin.end(JSON.stringify({ id: 0, cmd: 'engine.shutdown' }) + '\n'); } catch (e) { /* gone */ }
        setTimeout(function () { if (!exited) { child.kill('SIGKILL'); } }, 15000);
      }
    });
  });
}

function parseArgs(argv) {
  var o = {};
  for (var i = 0; i < argv.length; i++) {
    var k = argv[i];
    var v = argv[i + 1];
    if (k === '--engine') { o.engine = v; i++; } else if (k === '--lexicons') { o.lexicons = v; i++; } else if (k === '--data') { o.data = v; i++; } else if (k === '--port') { o.port = Number(v); i++; } else if (k === '--open-file') { o.openFile = v; i++; } else if (k === '--save-dir') { o.saveDir = v; i++; }
  }
  return o;
}

if (require.main === module) {
  var o = parseArgs(process.argv.slice(2));
  if (!o.port) { o.port = 8124; }
  start(o, function (err, s) {
    if (err) {
      process.stderr.write('engine_bridge_shim: ' + err.message + '\n');
      process.exit(1);
    }
    process.stdout.write('vetus poeta UI on the real engine: ' + s.url + 'index.html?debug=1   (engine log ' + s.logPath + '; Ctrl+C stops)\n');
    process.on('SIGINT', function () { s.close(function () { process.exit(0); }); });
  });
}

module.exports = { start: start };
