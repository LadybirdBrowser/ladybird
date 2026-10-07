// META: title=MessageChannel: port message queue is initially disabled

// TODO: duplicate of ./message-channels/no-start.any.js?

async_test(function(t) {
  var channel = new MessageChannel();
  channel.port2.addEventListener("message", t.unreached_func(), true);
  channel.port1.postMessage("ping");
  setTimeout(t.step_func_done(), 100);
});
