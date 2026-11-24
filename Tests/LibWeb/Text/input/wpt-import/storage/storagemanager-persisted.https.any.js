// META: title=StorageManager: persisted()

promise_test(function() {
    var promise = navigator.storage.persisted();
    assert_true(promise instanceof Promise,
        'navigator.storage.persisted() returned a Promise.');
    return promise.then(function (result) {
        assert_equals(typeof result, 'boolean', result + ' should be boolean');
    });
}, 'navigator.storage.persisted() returns a promise that resolves.');
