// META: title=IndexedDB: index renaming error handling
// META: global=window,worker
// META: script=resources/support-promises.js

// Spec: "https://w3c.github.io/IndexedDB/#dom-idbindex-name"

'use strict';

promise_test(testCase => {
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
                const store = transaction.objectStore('books');
                const index = store.index('by_author');
                store.deleteIndex('by_author');
                assert_throws_dom(
                    'InvalidStateError',
                    () => index.name = 'renamed_by_author');
              }))
      .then(database => database.close());
}, 'IndexedDB deleted index rename throws');

promise_test(testCase => {
  return createDatabase(testCase, (database, transaction) => {
           createBooksStore(testCase, database);
         }).then(database => {
    const transaction = database.transaction('books', 'readonly');
    const store = transaction.objectStore('books');
    const index = store.index('by_author');

    assert_throws_dom(
        'InvalidStateError', () => index.name = 'renamed_by_author');
    database.close();
  });
}, 'IndexedDB index rename throws in a readonly transaction');

promise_test(testCase => {
  return createDatabase(testCase, (database, transaction) => {
           createBooksStore(testCase, database);
         }).then(database => {
    const transaction = database.transaction('books', 'readwrite');
    const store = transaction.objectStore('books');
    const index = store.index('by_author');

    assert_throws_dom(
        'InvalidStateError', () => index.name = 'renamed_by_author');
    database.close();
  });
}, 'IndexedDB index rename throws in a readwrite transaction');

promise_test(testCase => {
  let authorIndex = null;
  return createDatabase(testCase, (database, transaction) => {
           const store = createBooksStore(testCase, database);
           authorIndex = store.index('by_author');
         }).then(database => {
    assert_throws_dom(
        'TransactionInactiveError',
        () => authorIndex.name = 'renamed_by_author');
    database.close();
  });
}, 'IndexedDB index rename throws in an inactive transaction');

promise_test(testCase => {
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
                const store = transaction.objectStore('books');
                const index = store.index('by_author');

                assert_throws_dom(
                    'ConstraintError', () => index.name = 'by_title');
                assert_array_equals(
                    store.indexNames, ['by_author', 'by_title'],
                    'An index rename that throws an exception should not change the ' +
                        'index\'s IDBObjectStore.indexNames');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_author', 'by_title'],
            'Committing a transaction with a failed store rename attempt ' +
                'should not change the index\'s IDBObjectStore.indexNames');
        const index = store.index('by_author');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Committing a transaction with a failed rename attempt should ' +
                       'not change the index\'s contents')
            .then(() => database.close());
      });
}, 'IndexedDB index rename to the name of another index throws');

promise_test(testCase => {
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
                const store = transaction.objectStore('books');
                const index = store.index('by_author');
                const exception = {name: 'Custom stringifying error'};
                assert_throws_exactly(exception, () => {
                  index.name = {
                    toString: () => {
                      throw exception;
                    }
                  };
                }, 'IDBObjectStore rename should re-raise toString() exception');
                assert_array_equals(
                    store.indexNames, ['by_author', 'by_title'],
                    'An index rename that throws an exception should not change the ' +
                        'index\'s IDBObjectStore.indexNames');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_author', 'by_title'],
            'Committing a transaction with a failed store rename attempt ' +
                'should not change the index\'s IDBObjectStore.indexNames');
        const index = store.index('by_author');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Committing a transaction with a failed rename attempt should ' +
                       'not change the index\'s contents')
            .then(() => database.close());
      });
}, 'IndexedDB index rename handles exceptions when stringifying names');
