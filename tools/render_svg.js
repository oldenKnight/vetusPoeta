/* render_svg.js - DEV/BUILD helper: rasterises SVG files (or small HTML pages) to PNG with
 * headless Chromium through Playwright. Used by tools/make_icons.py and tools/check_brand.js;
 * there is no cairo or Pillow here, so the browser is the rasteriser.
 *
 *   node tools/render_svg.js jobs.json
 *
 * jobs.json is an array of jobs, rendered in order in one browser:
 *   {"svg": "assets/logo.svg", "w": 256, "h": 256, "out": "assets/logo_256.png"}
 *   {"svg": "...", "w": 600, "h": 160, "bg": "#1B1815", "pad": 16, "out": "..."}
 *   {"html": "<!doctype html>...", "w": 1200, "h": 800, "out": "assets/splash.png"}
 * "svg" is a file path (relative to the current directory); the SVG fills a w x h box
 * (minus "pad" on every side) and keeps its aspect ratio. "bg" defaults to transparent
 * (the PNG keeps its alpha channel). device_scale_factor is 1, so 1 CSS px = 1 PNG px.
 *
 * Playwright comes from NODE_PATH, the global "playwright" module or
 * /opt/node-tools/node_modules; Chromium from VP_CHROMIUM, the Playwright default, or
 * $PLAYWRIGHT_BROWSERS_PATH (default /opt/pw-browsers), newest chromium-NNNN/chrome-linux/chrome.
 * Exit codes: 0 ok, 2 Playwright or Chromium missing (the message says which), 1 other error.
 */
'use strict';

var fs = require('fs');
var path = require('path');

function loadPlaywright() {
  var tries = [];
  if (process.env.NODE_PATH) {
    tries = tries.concat(process.env.NODE_PATH.split(path.delimiter).filter(Boolean).map(function (p) { return path.join(p, 'playwright'); }));
  }
  tries.push('playwright', '/opt/node-tools/node_modules/playwright');
  for (var i = 0; i < tries.length; i++) {
    try { return require(tries[i]); } catch (e) { /* next */ }
  }
  return null;
}

function findChromium() {
  if (process.env.VP_CHROMIUM && fs.existsSync(process.env.VP_CHROMIUM)) { return process.env.VP_CHROMIUM; }
  var base = process.env.PLAYWRIGHT_BROWSERS_PATH || '/opt/pw-browsers';
  if (!fs.existsSync(base)) { return null; }
  var dirs = fs.readdirSync(base).filter(function (d) { return /^chromium-\d+$/.test(d); }).sort().reverse();
  for (var i = 0; i < dirs.length; i++) {
    var exe = path.join(base, dirs[i], 'chrome-linux', 'chrome');
    if (fs.existsSync(exe)) { return exe; }
  }
  return null;
}

function pageFor(job) {
  if (job.html) { return job.html; }
  var svg = fs.readFileSync(job.svg, 'utf8').replace(/^<\?xml[^>]*\?>\s*/, '');
  var pad = job.pad || 0;
  return '<!doctype html><html><head><meta charset="utf-8"><style>' +
    'html,body{margin:0;padding:0;background:' + (job.bg || 'transparent') + ';}' +
    '#box{box-sizing:border-box;width:' + job.w + 'px;height:' + job.h + 'px;padding:' + pad + 'px;}' +
    '#box>svg{width:100%;height:100%;display:block;}' +
    '</style></head><body><div id="box">' + svg + '</div></body></html>';
}

function launch(pw) {
  var exe = findChromium();
  var attempts = [];
  if (exe) { attempts.push({ executablePath: exe }); }
  attempts.push({});
  var i = 0;
  function next(lastErr) {
    if (i >= attempts.length) { return Promise.reject(lastErr || new Error('no Chromium')); }
    var opts = attempts[i++];
    return pw.chromium.launch(opts).catch(function (e) { return next(e); });
  }
  return next(null);
}

function main() {
  var jobsFile = process.argv[2];
  if (!jobsFile) {
    process.stderr.write('usage: node tools/render_svg.js jobs.json\n');
    process.exit(1);
  }
  var jobs = JSON.parse(fs.readFileSync(jobsFile, 'utf8'));
  var pw = loadPlaywright();
  if (!pw) {
    process.stderr.write('render_svg: Playwright not found (set NODE_PATH to a node_modules with "playwright", ' +
      'or install it: npm install playwright)\n');
    process.exit(2);
  }
  var browser = null;
  launch(pw).catch(function (e) {
    process.stderr.write('render_svg: no usable Chromium (' + String(e && e.message || e).split('\n')[0] + '); ' +
      'set VP_CHROMIUM or PLAYWRIGHT_BROWSERS_PATH\n');
    process.exit(2);
  }).then(function (b) {
    browser = b;
    var chain = Promise.resolve();
    jobs.forEach(function (job) {
      chain = chain.then(function () {
        return browser.newPage({ viewport: { width: job.w, height: job.h }, deviceScaleFactor: 1 }).then(function (page) {
          return page.setContent(pageFor(job), { waitUntil: 'load' })
            .then(function () { return page.evaluate(function () { return document.fonts ? document.fonts.ready.then(function () { return 1; }) : 1; }); })
            .then(function () {
              return page.screenshot({ omitBackground: !job.bg && !job.html, clip: { x: 0, y: 0, width: job.w, height: job.h } });
            })
            .then(function (png) {
              fs.mkdirSync(path.dirname(path.resolve(job.out)), { recursive: true });
              fs.writeFileSync(job.out, png);
              process.stdout.write('  rendered ' + job.out + ' ' + job.w + 'x' + job.h + ' (' + png.length + ' bytes)\n');
              return page.close();
            });
        });
      });
    });
    return chain;
  }).then(function () {
    return browser.close();
  }).catch(function (e) {
    process.stderr.write('render_svg: ' + (e && e.stack || e) + '\n');
    if (browser) { browser.close(); }
    process.exit(1);
  });
}

main();
