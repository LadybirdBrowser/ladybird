setup({ allow_uncaught_exception: true });

var errors = 0;
window.onerror = function() {
  errors++;
};

test(function() {
  var e = document.body;
  e.setAttribute("onclick", "window.open(");
  assert_equals(e.onclick, null);
  assert_equals(e.onclick, null);
  assert_equals(errors, 1);
}, "Invalid uncompiled raw handlers should only be compiled once");
