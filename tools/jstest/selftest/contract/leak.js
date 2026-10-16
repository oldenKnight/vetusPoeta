// expect-contract: leaks globals: helper
(function () { window.VP_A = 1; window.helper = 2; }());
