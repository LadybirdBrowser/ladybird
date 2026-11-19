// Helper for tests that just want to verify the ordering of a series of events.
// Usage:
//   log_test(function(t, log) {
//      log('first');
//      log('second');
//   }, ['first', 'second'], 'Ordinal numbers are ordinal');

function log_test(func, expected, description) {
    async_test(function(t) {
        var actual = [];
        function log(entry) {
            actual.push(entry);
            if (expected.length <= actual.length) {
                assert_array_equals(actual, expected);
                t.done();
            }
        }
        func(t, t.step_func(log));
    }, description);
}
