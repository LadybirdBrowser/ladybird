// META: title=XMLHttpRequest: abort() after a timeout should not fire "abort" event

    var test = async_test();

    test.step(function() {
        // timeout is 100ms
        // the download would otherwise take 1000ms
        // we check after 300ms to make sure abort does not fire an "abort" event

        var timeoutFired = false;

        var client = new XMLHttpRequest();

        assert_true('timeout' in client, 'xhr.timeout is not supported in this user agent');

        client.timeout = 100;

        test.step_timeout(() => {
            assert_true(timeoutFired);

            // abort should not cause the "abort" event to fire
            client.abort();

            test.step_timeout(() => { // use a timeout to catch any implementation that might queue an abort event for later - just in case
              test.done()
            }, 200);

            assert_equals(client.readyState, 0);
        }, 300);

        client.ontimeout = function () {
            timeoutFired = true;
        };

        client.onabort = test.step_func(function () {
            // this should not fire!

            assert_unreached("abort() should not cause the abort event to fire");
        });

        client.open("GET", "/common/blank.html?pipe=trickle(d1)", true);
        client.send(null);
    });
