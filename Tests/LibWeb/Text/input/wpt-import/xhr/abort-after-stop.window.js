// META: title=XMLHttpRequest: abort event should fire when stop() method is used

      var test = async_test();
      window.onload = test.step_func(function() {
        var client = new XMLHttpRequest();
        var abortFired = false;
        var sync = true;
        client.onabort = test.step_func(function (e) {
          assert_false(sync);
          assert_equals(e.type, 'abort');
          assert_equals(client.status, 0);
          abortFired = true;
        });
        client.open("GET", "resources/delay.py?ms=3000", true);
        client.send(null);
        test.step_timeout(() => {
          assert_equals(abortFired, true);
          test.done();
        }, 200);
        window.stop();
        sync = false;
      });
