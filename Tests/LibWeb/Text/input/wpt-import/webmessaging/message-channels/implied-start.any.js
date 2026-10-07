// META: title=onmessage implied start()

async_test(function(t) {
  var channel = new MessageChannel();
  channel.port1.postMessage(1);
  channel.port2.onmessage = function() {
    setTimeout(t.step_func(function() {
      t.done();
    }), 50);
    channel.port2.onmessage = t.step_func(function() {
      assert_unreached();
    });
  }; // implies start()
});
