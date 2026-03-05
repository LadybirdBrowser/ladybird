// META: script=resources/support-promises.js
'use strict';

promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  objectStore.put({isbn: 'one', title: 'title1'});
  objectStore.put({isbn: 'two', title: 'title2'});
  objectStore.put({isbn: 'three', title: 'title3'});
  txn.commit();
  await promiseForTransaction(testCase, txn);

  const txn2 = db.transaction(['books'], 'readonly');
  const objectStore2 = txn2.objectStore('books');
  const getRequestitle1 = objectStore2.get('one');
  const getRequestitle2 = objectStore2.get('two');
  const getRequestitle3 = objectStore2.get('three');
  txn2.commit();
  await promiseForTransaction(testCase, txn2);
  assert_array_equals(
      [getRequestitle1.result.title,
          getRequestitle2.result.title,
          getRequestitle3.result.title],
      ['title1', 'title2', 'title3'],
      'All three retrieved titles should match those that were put.');
  db.close();
}, 'Explicitly committed data can be read back out.');


promise_test(async testCase => {
  let db = await createDatabase(testCase, () => {});
  assert_equals(1, db.version, 'A database should be created as version 1');
  db.close();

  // Upgrade the versionDB database and explicitly commit its versionchange
  // transaction.
  db = await migrateDatabase(testCase, 2, (db, txn) => {
    txn.commit();
  });
  assert_equals(2, db.version,
      'The database version should have been incremented regardless of '
      + 'whether the versionchange transaction was explicitly or implicitly '
      + 'committed.');
  db.close();
}, 'commit() on a version change transaction does not cause errors.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  txn.commit();
  assert_throws_dom('TransactionInactiveError',
      () => { objectStore.put({isbn: 'one', title: 'title1'}); },
      'After commit is called, the transaction should be inactive.');
  db.close();
}, 'A committed transaction becomes inactive immediately.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  const putRequest = objectStore.put({isbn: 'one', title: 'title1'});
  putRequest.onsuccess = testCase.step_func(() => {
    assert_throws_dom('TransactionInactiveError',
      () => { objectStore.put({isbn:'two', title:'title2'}); },
      'The transaction should not be active in the callback of a request after '
      + 'commit() is called.');
  });
  txn.commit();
  await promiseForTransaction(testCase, txn);
  db.close();
}, 'A committed transaction is inactive in future request callbacks.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  txn.commit();

  assert_throws_dom('TransactionInactiveError',
      () => { objectStore.put({isbn:'one', title:'title1'}); },
      'After commit is called, the transaction should be inactive.');

  const txn2 = db.transaction(['books'], 'readonly');
  const objectStore2 = txn2.objectStore('books');
  const getRequest = objectStore2.get('one');
  await promiseForTransaction(testCase, txn2);
  assert_equals(getRequest.result, undefined);

  db.close();
}, 'Puts issued after commit are not fulfilled.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  txn.abort();
  assert_throws_dom('InvalidStateError',
      () => { txn.commit(); },
      'The transaction should have been aborted.');
  db.close();
}, 'Calling commit on an aborted transaction throws.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  txn.commit();
  assert_throws_dom('InvalidStateError',
      () => { txn.commit(); },
      'The transaction should have already committed.');
  db.close();
}, 'Calling commit on a committed transaction throws.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  const putRequest = objectStore.put({isbn:'one', title:'title1'});
  txn.commit();
  assert_throws_dom('InvalidStateError',
      () => { txn.abort(); },
      'The transaction should already have committed.');
  const txn2 = db.transaction(['books'], 'readwrite');
  const objectStore2 = txn2.objectStore('books');
  const getRequest = objectStore2.get('one');
  await promiseForTransaction(testCase, txn2);
  assert_equals(
      getRequest.result.title,
      'title1',
      'Explicitly committed data should be gettable.');
  db.close();
}, 'Calling abort on a committed transaction throws and does not prevent '
   + 'persisting the data.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn = db.transaction(['books'], 'readwrite');
  const objectStore = txn.objectStore('books');
  const releaseTxnFunction = keepAlive(testCase, txn, 'books');

  // Break up the scope of execution to force the transaction into an inactive
  // state.
  await timeoutPromise(0);

  assert_throws_dom('InvalidStateError',
      () => { txn.commit(); },
      'The transaction should be inactive so calling commit should throw.');
  releaseTxnFunction();
  db.close();
}, 'Calling txn.commit() when txn is inactive should throw.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
    createNotBooksStore(testCase, db);
  });
  // Txn1 should commit before txn2, even though txn2 uses commit().
  const txn1 = db.transaction(['books'], 'readwrite');
  txn1.objectStore('books').put({isbn: 'one', title: 'title1'});
  const releaseTxnFunction = keepAlive(testCase, txn1, 'books');

  const txn2 = db.transaction(['books'], 'readwrite');
  txn2.objectStore('books').put({isbn:'one', title:'title2'});
  txn2.commit();

  // Exercise the IndexedDB transaction ordering by executing one with a
  // different scope. A readonly transaction is used here because
  // implementations are not required to run non-overlapping readwrite
  // transactions in parallel, and some implementations (ex: Firefox)
  // will not.
  const txn3 = db.transaction(['not_books'], 'readonly');
  txn3.objectStore('not_books').getAllKeys();
  txn3.oncomplete = function() {
    releaseTxnFunction();
  }
  await Promise.all([promiseForTransaction(testCase, txn1),
                     promiseForTransaction(testCase, txn2)]);

  // Read the data back to verify that txn2 executed last.
  const txn4 = db.transaction(['books'], 'readonly');
  const getRequest4 = txn4.objectStore('books').get('one');
  await promiseForTransaction(testCase, txn4);
  assert_equals(getRequest4.result.title, 'title2');
  db.close();
}, 'Transactions with same scope should stay in program order, even if one '
   + 'calls commit.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  // Txn1 creates the book 'one' so the 'add()' below fails.
  const txn1 = db.transaction(['books'], 'readwrite');
  txn1.objectStore('books').add({isbn:'one', title:'title1'});
  txn1.commit();
  await promiseForTransaction(testCase, txn1);

  // Txn2 should abort, because the 'add' call is invalid, and commit() was
  // called.
  const txn2 = db.transaction(['books'], 'readwrite');
  const objectStore2 = txn2.objectStore('books');
  objectStore2.put({isbn:'two', title:'title2'});
  const addRequest = objectStore2.add({isbn:'one', title:'title2'});
  txn2.commit();
  txn2.oncomplete = () => { assert_unreached(
    'Transaction with invalid "add" call should not be completed.'); };

  // Wait for the transaction to complete. We have to explicitly wait for the
  // error signal on the transaction because of the nature of the test tooling.
  await Promise.all([
      requestWatcher(testCase, addRequest).wait_for('error'),
      transactionWatcher(testCase, txn2).wait_for(['error', 'abort'])
  ]);

  // Read the data back to verify that txn2 was aborted.
  const txn3 = db.transaction(['books'], 'readonly');
  const objectStore3 = txn3.objectStore('books');
  const getRequest1 = objectStore3.get('one');
  const getRequest2 = objectStore3.count('two');
  await promiseForTransaction(testCase, txn3);
  assert_equals(getRequest1.result.title, 'title1');
  assert_equals(getRequest2.result, 0);
  db.close();
}, 'Transactions that explicitly commit and have errors should abort.');


promise_test(async testCase => {
  const db = await createDatabase(testCase, db => {
    createBooksStore(testCase, db);
  });
  const txn1 = db.transaction(['books'], 'readwrite');
  txn1.objectStore('books').add({isbn: 'one', title: 'title1'});
  txn1.commit();
  await promiseForTransaction(testCase, txn1);

  // The second add request will throw an error, but the onerror handler will
  // appropriately catch the error allowing the valid put request on the
  // transaction to commit.
  const txn2 = db.transaction(['books'], 'readwrite');
  const objectStore2 = txn2.objectStore('books');
  objectStore2.put({isbn: 'two', title:'title2'});
  const addRequest = objectStore2.add({isbn: 'one', title:'unreached_title'});
  addRequest.onerror = (event) => {
    event.preventDefault();
    addRequest.transaction.commit();
  };

  // Wait for the transaction to complete. We have to explicitly wait for the
  // error signal on the transaction because of the nature of the test tooling.
  await transactionWatcher(testCase,txn2).wait_for(['error', 'complete'])

  // Read the data back to verify that txn2 was committed.
  const txn3 = db.transaction(['books'], 'readonly');
  const objectStore3 = txn3.objectStore('books');
  const getRequest1 = objectStore3.get('one');
  const getRequest2 = objectStore3.get('two');
  await promiseForTransaction(testCase, txn3);
  assert_equals(getRequest1.result.title, 'title1');
  assert_equals(getRequest2.result.title, 'title2');
  db.close();
}, 'Transactions that handle all errors properly should behave as ' +
   'expected when an explicit commit is called in an onerror handler.');
