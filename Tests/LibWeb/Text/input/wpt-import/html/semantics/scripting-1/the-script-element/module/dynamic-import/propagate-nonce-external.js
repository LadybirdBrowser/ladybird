// This file is loaded both as a module and as a classic script.
promise_test(t => {
  return import("../imports-a.js").then(module => {
    assert_true(window.evaluated_imports_a);
    assert_equals(module.A["from"], "imports-a.js");
  });
}, "Dynamically imported module should eval when imported from script w/ a valid nonce.");
