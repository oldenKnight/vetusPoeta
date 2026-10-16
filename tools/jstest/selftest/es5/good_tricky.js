// expect:
/* Clean ES5 that contains banned text only inside strings, comments and regex literals:
   => ` let const class ... async await ** Object.assign Promise.resolve .includes( */
(function () {
  'use strict';
  var s = 'arrow => and `backtick` and let const class ... Promise.resolve(1)';
  var t = "for (var x of y) and a ** b and Object.assign({}, {})";
  var re = /=>|`|\.\.\.|\*\*/g;
  var slash = /[/]+/;
  var half = 10 / 2 / 1;
  var o = { 'class': 1, 'let': 2, value: half };
  var p = o.value;
  if (p) { o.count = 1; }
  while (p > 100) { p--; }
  function run(a, b) { return a / b; }
  var q = { go: function (a) { return a; } };
  return [s, t, re, slash, o, run, q];
}());
