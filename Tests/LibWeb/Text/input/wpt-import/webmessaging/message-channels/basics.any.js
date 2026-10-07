// META: title=basic messagechannel test

async_test(function(t) {
  var channel = new MessageChannel();
  channel.port1.postMessage(1);
  channel.port2.onmessage = t.step_func(
    function(e) {
      assert_equals(e.data, 1);
      t.done();
    });
  channel.port2.start();
});
