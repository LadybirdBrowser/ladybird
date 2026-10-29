// META: global=window

[
  "arrayBuffer",
  "blob",
  "bytes",
  "formData",
  "json",
  "text"
].forEach(method => {
  promise_test(t => {
    return fetch("resources/bad-br-body.py").then(res => {
      assert_equals(res.status, 200);
      return promise_rejects_js(t, TypeError, res[method]());
    });
  }, "Consuming the body of a resource with bad br content with " + method + "() should reject");
});
