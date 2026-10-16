// expect: eval innerHTML rawListener setInterval styleAttr
el.addEventListener("click", f);
setInterval(f, 10);
el.innerHTML = "<b>x</b>";
el.setAttribute("style", "color: red");
eval("1");
