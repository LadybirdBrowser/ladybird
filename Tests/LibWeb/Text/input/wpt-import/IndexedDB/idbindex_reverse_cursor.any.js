// META: title=Reverse Cursor Validity
// META: script=resources/support-promises.js
'use strict';

async function iterateAndReturnAllCursorResult(testCase, cursor) {
  return new Promise((resolve, reject) => {
    let results = [];
    cursor.onsuccess = testCase.step_func(function(e) {
      let cursor = e.target.result;
      if (!cursor) {
        resolve(results);
        return;
      }
      results.push(cursor.value);
      cursor.continue();
    });
    cursor.onerror = reject;
  });
}

promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    db.createObjectStore('objectStore', {keyPath: 'key'})
              .createIndex('index', 'indexedOn');
  });
  const txn1 = db.transaction(['objectStore'], 'readwrite');
  txn1.objectStore('objectStore').add({'key': 'firstItem', 'indexedOn': 3});
  const txn2 = db.transaction(['objectStore'], 'readwrite');
  txn2.objectStore('objectStore').put({'key': 'firstItem', 'indexedOn': -1});
  const txn3= db.transaction(['objectStore'], 'readwrite');
  txn3.objectStore('objectStore').add({'key': 'secondItem', 'indexedOn': 2});

  const txn4 = db.transaction(['objectStore'], 'readonly');
  const txnWaiter = promiseForTransaction(testCase, txn4);
  const cursor = txn4.objectStore('objectStore').index('index').openCursor(IDBKeyRange.bound(0, 10), "prev");
  let results = await iterateAndReturnAllCursorResult(testCase, cursor);

  assert_equals(results.length, 1);

  await txnWaiter;
  db.close();
}, 'Reverse cursor sees update from separate transactions.');

promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    db.createObjectStore('objectStore', {keyPath: 'key'})
              .createIndex('index', 'indexedOn');
  });
  const txn = db.transaction(['objectStore'], 'readwrite');
  txn.objectStore('objectStore').add({'key': '1', 'indexedOn': 2});
  txn.objectStore('objectStore').put({'key': '1', 'indexedOn': -1});
  txn.objectStore('objectStore').add({'key': '2', 'indexedOn': 1});

  const txn2 = db.transaction(['objectStore'], 'readonly');
  const txnWaiter = promiseForTransaction(testCase, txn2);
  const cursor = txn2.objectStore('objectStore').index('index').openCursor(IDBKeyRange.bound(0, 10), "prev");
  let results = await iterateAndReturnAllCursorResult(testCase, cursor);

  assert_equals(1, results.length);

  await txnWaiter;
  db.close();
}, 'Reverse cursor sees in-transaction update.');
