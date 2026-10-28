
function testResultCallbackHandler(event) {
    if (event.data == "done") {
        done();
        return;
    }
    if (event.data.type == "is") {
        test(function() { assert_equals(event.data.got, event.data.expected); }, "Timeout test: " + event.data.msg);
        return;
    }
    if (event.data.type == "ok") {
        test(function() { assert_true(event.data.bool); }, "Timeout test: " + event.data.msg);
        return;
    }
}

window.addEventListener("message", testResultCallbackHandler);

// Setting up testharness.js
setup({ explicit_done: true });

