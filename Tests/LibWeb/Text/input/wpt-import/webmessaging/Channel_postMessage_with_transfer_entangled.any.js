async_test(function(t) {
    var channel1 = new MessageChannel();
    var channel2 = new MessageChannel();

     // One, send a message.
     channel1.port1.postMessage(1);

     // Two, transfer both ports.
     channel2.port1.postMessage("transfer", [channel1.port1]);
     channel2.port1.postMessage("transfer", [channel1.port2]);

     var transfer_counter = 0;
     var sender;
     channel2.port2.onmessage = t.step_func(function (evt) {
         if (transfer_counter == 0) {
             sender = evt.ports[0];
             transfer_counter = 1;
         } else {
             sender.postMessage(2);
             var counter = 0;
             evt.ports[0].onmessage = t.step_func(function (evt) {
                 if (counter == 0) {
                     assert_equals(evt.data, 1);
                     counter = 1;
                 } else if (counter == 1) {
                     assert_equals(evt.data, 2);
                     counter = 2;
                 } else {
                     assert_equals(evt.data, 3);
                     t.done();
                 }
             });
             sender.postMessage(3);
         }
      });
}, "An entangled port transferred to the same origin receives messages in order");
