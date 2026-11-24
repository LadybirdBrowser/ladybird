// META: title=StorageManager: persisted()

test(function(t) {
  assert_true('persisted' in navigator.storage);
  assert_equals(typeof navigator.storage.persisted, 'function');
  assert_true(navigator.storage.persisted() instanceof Promise);
}, 'persisted() method exists and returns a Promise');

promise_test(function(t) {
  return navigator.storage.persisted().then(function(result) {
    assert_equals(typeof result, 'boolean');
    assert_equals(result, false);
  });
}, 'persisted() returns a promise and resolves as boolean with false');
