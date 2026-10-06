// expect-contract: leaks globals: stray
(function () { window.VP_A = 1; }());
var stray = 1;
