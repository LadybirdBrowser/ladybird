// META: title=IndexedDB Transaction - active flag is set during event dispatch
// META: global=window,worker
// META: script=resources/support.js

'use strict';

function createObjectStore() {
  return (t, db) => {
    db.createObjectStore('store');
  };
}

function initializeTransaction(t, db, mode = 'readonly') {
  const tx = db.transaction('store', mode);
  const release_tx = keep_alive(tx, 'store');
  assert_true(
      is_transaction_active(tx, 'store'),
      'Transaction should be active after creation');
  return {tx, release_tx};
}

function assertLifetimeInMicrotasksAndEventLoop(
    t, tx, release_tx, handlerMessage) {
  assert_true(is_transaction_active(tx, 'store'), handlerMessage);

  let saw_promise = false;
  Promise.resolve().then(t.step_func(() => {
    saw_promise = true;
    assert_true(
        is_transaction_active(tx, 'store'),
        'Transaction should be active in microtasks');
  }));

  setTimeout(
      t.step_func(() => {
        assert_true(saw_promise);
        assert_false(
            is_transaction_active(tx, 'store'),
            'Transaction should be inactive in next task');
        release_tx();
        t.done();
      }),
      0);
};

indexeddb_test(createObjectStore(), (t, db) => {
  const {tx, release_tx} = initializeTransaction(t, db);
  const request = tx.objectStore('store').get(0);
  request.onerror = t.unreached_func('request should succeed');
  request.onsuccess = t.step_func((e) => {
    assertLifetimeInMicrotasksAndEventLoop(
        t, tx, release_tx,
        'Transaction should be active during success handler');
  });
}, 'Active during success handlers');

indexeddb_test(createObjectStore(), (t, db) => {
  const {tx, release_tx} = initializeTransaction(t, db);
  const request = tx.objectStore('store').get(0);
  request.onerror = t.unreached_func('request should succeed');
  request.onsuccess = t.step_func((e) => {
    assertLifetimeInMicrotasksAndEventLoop(
        t, tx, release_tx,
        'Transaction should be active during success listener');
  });
}, 'Active during success listeners');

indexeddb_test(createObjectStore(), (t, db) => {
  const {tx, release_tx} = initializeTransaction(t, db, 'readwrite');
  tx.objectStore('store').put(0, 0);
  const request = tx.objectStore('store').add(0, 0);
  request.onsuccess = t.unreached_func('request should fail');
  request.onerror = t.step_func((e) => {
    e.preventDefault();
    assertLifetimeInMicrotasksAndEventLoop(
        t, tx, release_tx, 'Transaction should be active during error handler');
  });
}, 'Active during error handlers');

indexeddb_test(createObjectStore(), (t, db) => {
  const {tx, release_tx} = initializeTransaction(t, db, 'readwrite');
  tx.objectStore('store').put(0, 0);
  const request = tx.objectStore('store').add(0, 0);
  request.onsuccess = t.unreached_func('request should fail');
  request.onerror = t.step_func((e) => {
    e.preventDefault();
    assertLifetimeInMicrotasksAndEventLoop(
        t, tx, release_tx,
        'Transaction should be active during error listener');
  });
}, 'Active during error listeners');
