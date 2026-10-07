async_test(function(t) {
  var channel1 = new MessageChannel();
  var channel2 = new MessageChannel();
  var channel3 = new MessageChannel();
  channel2.port2.onmessage = t.step_func(function (evt) {
    channel3.port1.onmessage = t.step_func(function (evt) {
      var counter = 0;
      evt.ports[0].onmessage = t.step_func(function (evt) {
        if (counter == 0) {
          assert_equals(evt.data, "First");
          counter = 1;
        } else if (counter == 1) {
          assert_equals(evt.data, "Second");
          counter = 2;
        } else if (counter == 2) {
          assert_equals(evt.data, "Third");
          counter = 3;
        } else if (counter == 3) {
          assert_equals(evt.data, "Fourth");
          t.done();
        }
      });
      channel1.port2.postMessage("Fourth");
    });
    channel1.port2.postMessage("Second");
    channel1.port2.postMessage("Third");
    channel3.port2.postMessage("2", evt.ports);
  });
  channel1.port2.postMessage("First");
  channel2.port1.postMessage("1", [channel1.port1]);
}, `When transferring a non-enabled port mutiple times,
    incoming messages sent at various transfer steps are received in order upon enablement.`);
