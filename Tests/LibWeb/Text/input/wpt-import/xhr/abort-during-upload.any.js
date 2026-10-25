// META: title=XMLHttpRequest: abort() while sending data
// META: script=resources/xmlhttprequest-event-order.js

      var test = async_test()
      test.step(function() {
        var client = new XMLHttpRequest()
        prepare_xhr_for_event_order_test(client);
        client.open("POST", "resources/delay.py?ms=1000")
        client.addEventListener("loadend", function(e) {
          test.step(function() {
            assert_xhr_event_order_matches([1, "loadstart(0,0,false)", "upload.loadstart(0,9999,true)", 4, "upload.abort(0,0,false)", "upload.loadend(0,0,false)", "abort(0,0,false)", "loadend(0,0,false)"]);
            test.done()
          })
        });
        client.send((new Array(10000)).join('a'))
        client.abort()
      })
