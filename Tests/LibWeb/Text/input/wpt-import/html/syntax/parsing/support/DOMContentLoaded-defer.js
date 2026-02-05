t.step(function() {
  assert_false(dcl, "DOMContentLoaded should not have fired before executing " +
                    "a defer script");

  t.step_timeout(function() {
    assert_false(dcl, "DOMContentLoaded should not have fired before " +
                      "executing a task queued from a defer script");
    t.step_timeout(function() {
      assert_true(dcl, "DOMContentLoaded should have fired in a task that " +
                       "was queued after the DOMContentLoaded task was queued");
      t.done();
    }, 0);
  }, 0);
});
