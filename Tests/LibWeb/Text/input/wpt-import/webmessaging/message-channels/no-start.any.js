// META: title=without start()

async_test(function(t) {
  var channel = new MessageChannel();
  channel.port1.postMessage(1);
  var i = 0;
  channel.port2.addEventListener('message', function() { i++; }, false);
  setTimeout(t.step_func(function() { assert_equals(i, 0); t.done();}), 50);
});
