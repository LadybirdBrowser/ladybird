setup({ allow_uncaught_exception:true });

[
  1,
  new TypeError(),
  undefined
].forEach(throwable => {
  test(t => {
    let happened = false;
    self.addEventListener("error", t.step_func(e => {
      assert_true(e.message !== "");
      assert_equals(e.filename, new URL("reporterror.any.js", location.href).href);
      assert_greater_than(e.lineno, 0);
      assert_greater_than(e.colno, 0);
      assert_equals(e.error, throwable);
      happened = true;
    }), { once:true });
    self.reportError(throwable);
    assert_true(happened);
  }, `self.reportError(${throwable})`);
});

test(() => {
  assert_throws_js(TypeError, () => self.reportError());
}, `self.reportError() (without arguments) throws`);

test(() => {
  // Workaround for https://github.com/web-platform-tests/wpt/issues/32105
  let invoked = false;
  self.reportError({
    get name() {
      invoked = true;
      assert_unreached('get name')
    },
    get message() {
      invoked = true;
      assert_unreached('get message');
    },
    get fileName() {
      invoked = true;
      assert_unreached('get fileName');
    },
    get lineNumber() {
      invoked = true;
      assert_unreached('get lineNumber');
    }
  });
  assert_false(invoked);
}, `self.reportError() doesn't invoke getters`);
