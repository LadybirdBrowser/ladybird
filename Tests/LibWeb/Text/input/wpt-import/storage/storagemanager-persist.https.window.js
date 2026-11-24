// META: title=StorageManager: persist()
// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js
// META: script=resources/helpers.js

promise_setup(async () => {
    await tryDenyingPermission();
});

promise_test(function() {
    var promise = navigator.storage.persist();
    assert_true(promise instanceof Promise,
        'navigator.storage.persist() returned a Promise.');
    return promise.then(function(result) {
        assert_equals(typeof result, 'boolean', result + ' should be boolean');
    });
}, 'navigator.storage.persist() returns a promise that resolves.');
