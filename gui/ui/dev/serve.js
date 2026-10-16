/* serve.js - DEV ONLY: serves gui/ui over http://127.0.0.1 so the page can load its JSON
 * string tables and fonts (browsers block both from file:// pages).
 *   node gui/ui/dev/serve.js [port]   then open  http://127.0.0.1:8123/index.html?mock=1
 * Used by smoke.js. Not part of the app bundle.
 */
'use strict';

var http = require('http');
var fs = require('fs');
var path = require('path');

var UI = path.resolve(__dirname, '..');
var TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8', '.ttf': 'font/ttf', '.svg': 'image/svg+xml', '.txt': 'text/plain; charset=utf-8',
  '.png': 'image/png', '.ico': 'image/x-icon'
};

function start(port, cb) {
  var server = http.createServer(function (req, res) {
    var url;
    try {
      url = decodeURIComponent(String(req.url).split('?')[0]);
    } catch (e) {
      res.writeHead(400);
      res.end();
      return;
    }
    if (url === '/') { url = '/index.html'; }
    var file = path.normalize(path.join(UI, url));
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
      res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
      res.end(data);
    });
  });
  server.on('error', function (e) { cb(e); });
  server.listen(port || 0, '127.0.0.1', function () {
    var base = 'http://127.0.0.1:' + server.address().port + '/';
    cb(null, { url: base, close: function (done) { server.close(done); } });
  });
}

if (require.main === module) {
  start(Number(process.argv[2]) || 8123, function (err, s) {
    if (err) {
      process.stderr.write('serve: ' + err.message + '\n');
      process.exit(1);
    }
    process.stdout.write('vetus poeta UI: ' + s.url + 'index.html?mock=1&debug=1   (Ctrl+C stops)\n');
  });
}

module.exports = { start: start, UI: UI };
