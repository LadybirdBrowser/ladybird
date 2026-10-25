// META: title=XMLHttpRequest: The send() method: Fire a progress event named loadstart on upload object (synchronous flag is unset)

var test = async_test();
test.step(function () {
  var client = new XMLHttpRequest();
  client.upload.onloadstart = test.step_func(function (e) {
    assert_true(e instanceof ProgressEvent);
    assert_equals(e.total, 7, 'upload.onloadstart: event.total');
    assert_equals(e.loaded, 0, 'upload.onloadstart: event.loaded');
    assert_equals(e.type, "loadstart");
    test.done();
  });
  client.onreadystatechange = test.step_func(function () {
    if (client.readyState === 4)
      assert_unreached("onloadstart not called.");
  });
  client.open("POST", "resources/trickle.py?ms=5&count=8");
  client.send('foo=bar');
});
