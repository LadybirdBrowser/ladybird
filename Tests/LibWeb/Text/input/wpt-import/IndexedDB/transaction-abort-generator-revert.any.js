// META: title=IndexedDB: aborting transactions reverts an object store's key generator state
// META: global=window,worker
// META: script=resources/support-promises.js
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#abort-transaction

'use strict';

promise_test(
    testCase => {
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                 })
          .then(database => {
            database.close();
          })
          .then(() => {
            return new Promise((resolve, reject) => {
              const request = indexedDB.open(databaseName(testCase), 2);
              request.onupgradeneeded = testCase.step_func(event => {
                const database = event.target.result;
                const transaction = event.target.transaction;
                const store = transaction.objectStore('books');
                const request2 =
                    store.put({title: 'Bedrock Nights II', author: 'Barney'});
                request2.onerror = testCase.unreached_func(
                    'IDBObjectStore.put() should not receive an error request');
                request2.onsuccess = testCase.step_func(event => {
                  assert_equals(
                      event.target.result, 345679,
                      'The key generator\'s current number should be set by ' +
                          'the last put operation in the database creation ' +
                          'transaction');

                  request.onerror = event => {
                    event.preventDefault();
                    resolve(event);
                  };
                  request.onsuccess = () => reject(new Error(
                      'indexedDB.open should not succeed after the ' +
                      'versionchange transaction is aborted'));

                  transaction.abort();
                });
              });
              request.onerror = event => reject(event.target.error);
              request.onsuccess = () => reject(new Error(
                  'indexedDB.open should not succeed without creating a ' +
                  'versionchange transaction'));
            });
          })
          .then(() => {
            return openDatabase(testCase, 1);
          })
          .then(database => {
            const transaction = database.transaction(['books'], 'readwrite');
            const store = transaction.objectStore('books');

            return checkStoreGenerator(
                       testCase, store, 345679,
                       'The key generator\'s current number should be reverted after the ' +
                           'transaction modifying it is aborted')
                .then(() => database.close());
          });
    },
    'The current number of a key generator is reverted when a versionchange ' +
        'transaction aborts');

promise_test(
    testCase => {
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                 })
          .then(database => {
            return new Promise((resolve, reject) => {
                     const transaction =
                         database.transaction(['books'], 'readwrite');
                     const store = transaction.objectStore('books');
                     const request = store.put(
                         {title: 'Bedrock Nights II', author: 'Barney'});
                     request.onerror = testCase.unreached_func(
                         'IDBObjectStore.put() should not receive an error request');
                     request.onsuccess = testCase.step_func(event => {
                       assert_equals(
                           event.target.result, 345679,
                           'The key generator\'s current number should be set by the ' +
                               'last put operation in the database creation transaction');

                       transaction.onabort = event => {
                         event.preventDefault();
                         resolve(event);
                       };
                       transaction.abort();
                     });
                     transaction.onabort = () => reject(new Error(
                         'The aborted readwrite transaction should not receive an ' +
                         'abort event before IDBTransaction.abort() is called'));
                     transaction.oncomplete = () => reject(new Error(
                         'The aborted readwrite transaction should not receive a ' +
                         'completed event'));
                     transaction.onerror = () => reject(new Error(
                         'The aborted readwrite transaction should not receive an ' +
                         'error event'));
                   })
                .then(() => database);
          })
          .then(database => {
            const transaction = database.transaction(['books'], 'readwrite');
            const store = transaction.objectStore('books');

            return checkStoreGenerator(
                       testCase, store, 345679,
                       'The key generator\'s current number should be reverted after the ' +
                           'transaction modifying it is aborted')
                .then(() => database.close());
          });
    },
    'The current number of a key generator is reverted when a readwrite ' +
        'transaction aborts');
