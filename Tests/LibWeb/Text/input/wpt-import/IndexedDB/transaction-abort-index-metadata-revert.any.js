// META: title=IndexedDB: aborting transactions reverts index metadata
// META: global=window,worker
// META: script=resources/support-promises.js
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#abort-transaction

'use strict';

promise_test(
    testCase => {
      let store = null;
      let index = null;
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                 })
          .then(database => {
            database.close();
          })
          .then(
              () => migrateDatabase(
                  testCase, 2,
                  (database, transaction) => {
                    store = createNotBooksStore(testCase, database);
                    index = store.index('not_by_author');
                    assert_array_equals(
                        store.indexNames, ['not_by_author', 'not_by_title'],
                        'IDBObjectStore.indexNames should include newly created indexes ' +
                            'before the transaction is aborted');

                    transaction.abort();
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is marked for deletion, immediately after ' +
                            'IDBTransaction.abort() returns');
                    assert_array_equals(
                        store.indexNames, [],
                        'IDBObjectStore.indexNames should stop including the newly ' +
                            'created indexes immediately after IDBTransaction.abort() returns');
                  }))
          .then(() => {
            assert_throws_dom(
                'InvalidStateError', () => index.get('query'),
                'IDBIndex.get should throw InvalidStateError, indicating that ' +
                    'the index is marked for deletion, after the transaction is ' +
                    'aborted');
            assert_array_equals(
                store.indexNames, [],
                'IDBObjectStore.indexNames should stop including the newly ' +
                    'created indexes after the transaction is aborted');
          });
    },
    'Created stores get their indexes marked as deleted after the transaction ' +
        'that created them aborts');

promise_test(
    testCase => {
      let store = null;
      let index = null;
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                   createNotBooksStore(testCase, database);
                 })
          .then(database => {
            database.close();
          })
          .then(
              () => migrateDatabase(
                  testCase, 2,
                  (database, transaction) => {
                    store = transaction.objectStore('not_books');
                    index = store.index('not_by_author');

                    database.deleteObjectStore('not_books');
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is marked for deletion, immediately after ' +
                            'IDBDatabase.deleteObjectStore() returns');
                    assert_array_equals(
                        store.indexNames, [],
                        'IDBObjectStore.indexNames should be empty immediately after ' +
                            'IDBDatabase.deleteObjectStore() returns');

                    transaction.abort();
                    assert_throws_dom(
                        'TransactionInactiveError', () => index.get('query'),
                        'IDBIndex.get should throw TransactionInactiveError, indicating ' +
                            'that the index is no longer marked for deletion, immediately ' +
                            'after IDBTransaction.abort() returns');
                    assert_array_equals(
                        store.indexNames, ['not_by_author', 'not_by_title'],
                        'IDBObjectStore.indexNames should include the deleted indexes ' +
                            'immediately after IDBTransaction.abort() returns');
                  }))
          .then(() => {
            assert_throws_dom(
                'TransactionInactiveError', () => index.get('query'),
                'IDBIndex.get should throw TransactionInactiveError, indicating ' +
                    'that the index is no longer marked for deletion, after the ' +
                    'transaction is aborted');
            assert_array_equals(
                store.indexNames, ['not_by_author', 'not_by_title'],
                'IDBObjectStore.indexNames should include the deleted indexes ' +
                    'after the transaction is aborted');
          });
    },
    'Deleted stores get their indexes marked as not-deleted after the ' +
        'transaction that deleted them aborts');

promise_test(
    testCase => {
      let store = null;
      let index = null;
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                 })
          .then(database => {
            database.close();
          })
          .then(
              () => migrateDatabase(
                  testCase, 2,
                  (database, transaction) => {
                    store = createNotBooksStore(testCase, database);
                    index = store.index('not_by_author');
                    assert_array_equals(
                        store.indexNames, ['not_by_author', 'not_by_title'],
                        'IDBObjectStore.indexNames should include newly created indexes ' +
                            'before the transaction is aborted');

                    database.deleteObjectStore('not_books');
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is marked for deletion, immediately after ' +
                            'IDBDatabase.deleteObjectStore() returns');
                    assert_array_equals(
                        store.indexNames, [],
                        'IDBObjectStore.indexNames should be empty immediately after ' +
                            'IDBDatabase.deleteObjectStore() returns');

                    transaction.abort();
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is still marked for deletion, immediately after ' +
                            'IDBTransaction.abort() returns');
                    assert_array_equals(
                        store.indexNames, [],
                        'IDBObjectStore.indexNames should not include the newly ' +
                            'created indexes immediately after IDBTransaction.abort() returns');
                  }))
          .then(() => {
            assert_throws_dom(
                'InvalidStateError', () => index.get('query'),
                'IDBIndex.get should throw InvalidStateError, indicating that ' +
                    'the index is still marked for deletion, after the transaction ' +
                    'is aborted');
            assert_array_equals(
                store.indexNames, [],
                'IDBObjectStore.indexNames should not include the newly ' +
                    'created indexes after the transaction is aborted');
          });
    },
    'Created+deleted stores still have their indexes marked as deleted after ' +
        'the transaction aborts');

promise_test(testCase => {
  let store = null;
  let index = null;
  return createDatabase(
             testCase,
             (database, transaction) => {
               createBooksStore(testCase, database);
               createNotBooksStore(testCase, database);
             })
      .then(database => {
        database.close();
      })
      .then(
          () => migrateDatabase(
              testCase, 2,
              (database, transaction) => {
                store = transaction.objectStore('not_books');
                index = store.createIndex('not_by_isbn', 'isbn');
                assert_array_equals(
                    store.indexNames,
                    ['not_by_author', 'not_by_isbn', 'not_by_title'],
                    'IDBObjectStore.indexNames should include newly created indexes ' +
                        'before the transaction is aborted');

                transaction.abort();
                assert_throws_dom(
                    'InvalidStateError', () => index.get('query'),
                    'IDBIndex.get should throw InvalidStateError, indicating that ' +
                        'the index is marked for deletion, immediately after ' +
                        'IDBTransaction.abort() returns');
                assert_array_equals(
                    store.indexNames, ['not_by_author', 'not_by_title'],
                    'IDBObjectStore.indexNames should stop including the newly ' +
                        'created index immediately after IDBTransaction.abort() returns');
              }))
      .then(() => {
        assert_throws_dom(
            'InvalidStateError', () => index.get('query'),
            'IDBIndex.get should throw InvalidStateError, indicating that ' +
                'the index is marked for deletion, after the transaction is ' +
                'aborted');
        assert_array_equals(
            store.indexNames, ['not_by_author', 'not_by_title'],
            'IDBObjectStore.indexNames should stop including the newly ' +
                'created index after the transaction is aborted');
      });
}, 'Created indexes get marked as deleted after their transaction aborts');

promise_test(testCase => {
  let store = null;
  let index = null;
  return createDatabase(
             testCase,
             (database, transaction) => {
               createBooksStore(testCase, database);
               createNotBooksStore(testCase, database);
             })
      .then(database => {
        database.close();
      })
      .then(
          () => migrateDatabase(
              testCase, 2,
              (database, transaction) => {
                store = transaction.objectStore('not_books');
                index = store.index('not_by_author');

                store.deleteIndex('not_by_author');
                assert_throws_dom(
                    'InvalidStateError', () => index.get('query'),
                    'IDBIndex.get should throw InvalidStateError, indicating that ' +
                        'the index is marked for deletion, immediately after ' +
                        'IDBObjectStore.deleteIndex() returns');
                assert_array_equals(
                    store.indexNames, ['not_by_title'],
                    'IDBObjectStore.indexNames should not include the deleted index ' +
                        'immediately after IDBObjectStore.deleteIndex() returns');

                transaction.abort();
                assert_throws_dom(
                    'TransactionInactiveError', () => index.get('query'),
                    'IDBIndex.get should throw TransactionInactiveError, indicating ' +
                        'that the index is no longer marked for deletion, immediately ' +
                        'after IDBTransaction.abort() returns');
                assert_array_equals(
                    store.indexNames, ['not_by_author', 'not_by_title'],
                    'IDBObjectStore.indexNames should include the deleted indexes ' +
                        'immediately after IDBTransaction.abort() returns');
              }))
      .then(() => {
        assert_throws_dom(
            'TransactionInactiveError', () => index.get('query'),
            'IDBIndex.get should throw TransactionInactiveError, indicating ' +
                'that the index is no longer marked for deletion, after the ' +
                'transaction is aborted');
        assert_array_equals(
            store.indexNames, ['not_by_author', 'not_by_title'],
            'IDBObjectStore.indexNames should include the deleted indexes ' +
                'after the transaction is aborted');
      });
}, 'Deleted indexes get marked as not-deleted after the transaction aborts');

promise_test(
    testCase => {
      let store = null;
      let index = null;
      return createDatabase(
                 testCase,
                 (database, transaction) => {
                   createBooksStore(testCase, database);
                   createNotBooksStore(testCase, database);
                 })
          .then(database => {
            database.close();
          })
          .then(
              () => migrateDatabase(
                  testCase, 2,
                  (database, transaction) => {
                    store = transaction.objectStore('not_books');
                    index = store.createIndex('not_by_isbn', 'isbn');
                    assert_array_equals(
                        store.indexNames,
                        ['not_by_author', 'not_by_isbn', 'not_by_title'],
                        'IDBObjectStore.indexNames should include newly created indexes ' +
                            'before the transaction is aborted');

                    store.deleteIndex('not_by_isbn');
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is marked for deletion, immediately after ' +
                            'IDBObjectStore.deleteIndex() returns');
                    assert_array_equals(
                        store.indexNames, ['not_by_author', 'not_by_title'],
                        'IDBObjectStore.indexNames should not include the deleted index ' +
                            'immediately after IDBObjectStore.deleteIndex() returns');

                    transaction.abort();
                    assert_throws_dom(
                        'InvalidStateError', () => index.get('query'),
                        'IDBIndex.get should throw InvalidStateError, indicating that ' +
                            'the index is still marked for deletion, immediately after ' +
                            'IDBTransaction.abort() returns');
                    assert_array_equals(
                        store.indexNames, ['not_by_author', 'not_by_title'],
                        'IDBObjectStore.indexNames should stop including the newly ' +
                            'created index immediately after IDBTransaction.abort() returns');
                  }))
          .then(() => {
            assert_throws_dom(
                'InvalidStateError', () => index.get('query'),
                'IDBIndex.get should throw InvalidStateError, indicating that ' +
                    'the index is marked for deletion, after the transaction is ' +
                    'aborted');
            assert_array_equals(
                store.indexNames, ['not_by_author', 'not_by_title'],
                'IDBObjectStore.indexNames should stop including the newly ' +
                    'created index after the transaction is aborted');
          });
    },
    'Created+deleted indexes are still marked as deleted after their ' +
        'transaction aborts');
