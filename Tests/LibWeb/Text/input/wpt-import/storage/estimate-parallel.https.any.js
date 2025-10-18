// META: title=StorageManager: multiple estimate() calls in parallel

promise_test(async t => {
  let r1, r2;
  await Promise.all([
    navigator.storage.estimate().then(r => { r1 = r; }),
    navigator.storage.estimate().then(r => { r2 = r; })
  ]);
  assert_true(('usage' in r1) && ('quota' in r1),
              'first response should have expected fields');
  assert_true(('usage' in r2) && ('quota' in r2),
              'second response should have expected fields');
}, 'Multiple estimate() calls in parallel should complete');
