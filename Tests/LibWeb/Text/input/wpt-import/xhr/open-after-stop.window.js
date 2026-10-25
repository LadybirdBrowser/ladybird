// window.stop() below prevents the load event from firing, so wait until it is
// fired to start the test.
setup({explicit_done: true });

onload = () => {
  async_test(function(t) {
    const client = new XMLHttpRequest();

    const result = [];
    const expected = [
      'readystatechange', 0, 1,  // open()
    ];

    let state = 0;

    client.onreadystatechange = t.step_func(() => {
      result.push('readystatechange', state, client.readyState);
    });
    client.onabort = t.unreached_func("abort should not be fired after window.stop() and open()");
    client.onloadend = t.unreached_func("loadend should not be fired after window.stop() and open()");

    client.open("GET", "resources/well-formed.xml");
    assert_equals(client.readyState, 1);

    state = 1;
    client.send(null);
    state = 2;
    window.stop();
    // Unlike client.abort(), window.stop() does not change readyState
    // immediately, rather through a task...
    assert_equals(client.readyState, 1);
    state = 3;
    // ... which is then canceled when we open a new request anyway.
    client.open("GET", "resources/well-formed.xml");
    assert_equals(client.readyState, 1);
    assert_array_equals(result, expected);

    // Give the abort and loadend events a chance to fire (erroneously) before
    // calling this a success.
    t.step_timeout(t.step_func_done(), 1000);
  }, "open() after window.stop()");
  done();
};
