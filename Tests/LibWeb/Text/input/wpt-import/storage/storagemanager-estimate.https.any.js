// META: title=StorageManager: estimate()

test(t => {
  assert_true('estimate' in navigator.storage);
  assert_equals(typeof navigator.storage.estimate, 'function');
  assert_true(navigator.storage.estimate() instanceof Promise);
}, 'estimate() method exists and returns a Promise');

promise_test(async t => {
  const result = await navigator.storage.estimate();
  assert_equals(typeof result, 'object');
  assert_true('usage' in result);
  assert_equals(typeof result.usage, 'number');
  assert_true('quota' in result);
  assert_equals(typeof result.quota, 'number');
}, 'estimate() resolves to dictionary with members');
