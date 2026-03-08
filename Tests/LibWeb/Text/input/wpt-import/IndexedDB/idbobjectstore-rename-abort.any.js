// META: global=window,worker
// META: title=IndexedDB: object store renaming support in aborted transactions
// META: script=resources/support-promises.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-name

'use strict';

promise_test(testCase => {
  const dbName = databaseName(testCase);
  let bookStore = null;
  let bookStore2 = null;
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
                bookStore = transaction.objectStore('books');
                bookStore.name = 'renamed_books';

                transaction.abort();

                assert_equals(
                    bookStore.name, 'books',
                    'IDBObjectStore.name should not reflect the rename any more ' +
                        'immediately after transaction.abort() returns');
                assert_array_equals(
                    database.objectStoreNames, ['books'],
                    'IDBDatabase.objectStoreNames should not reflect the rename ' +
                        'any more immediately after transaction.abort() returns');
                assert_array_equals(
                    transaction.objectStoreNames, ['books'],
                    'IDBTransaction.objectStoreNames should not reflect the ' +
                        'rename any more immediately after transaction.abort() returns');
              }))
      .then(event => {
        assert_equals(
            bookStore.name, 'books',
            'IDBObjectStore.name should not reflect the rename any more ' +
                'after the versionchange transaction is aborted');
        const request = indexedDB.open(dbName, 1);
        return promiseForRequest(testCase, request);
      })
      .then(database => {
        assert_array_equals(
            database.objectStoreNames, ['books'],
            'IDBDatabase.objectStoreNames should not reflect the rename ' +
                'after the versionchange transaction is aborted');

        const transaction = database.transaction('books', 'readonly');
        bookStore2 = transaction.objectStore('books');
        return checkStoreContents(
                   testCase, bookStore2,
                   'Aborting an object store rename transaction should not change ' +
                       'the store\'s records')
            .then(() => database.close());
      })
      .then(() => {
        assert_equals(
            bookStore.name, 'books',
            'IDBObjectStore used in aborted rename transaction should not ' +
                'reflect the rename after the transaction is aborted');
        assert_equals(
            bookStore2.name, 'books',
            'IDBObjectStore obtained after an aborted rename transaction ' +
                'should not reflect the rename');
      });
}, 'IndexedDB object store rename in aborted transaction');

promise_test(testCase => {
  const dbName = databaseName(testCase);
  let notBookStore = null;
  return createDatabase(testCase, (database, transaction) => {})
      .then(database => {
        database.close();
      })
      .then(
          () => migrateDatabase(
              testCase, 2,
              (database, transaction) => {
                notBookStore = createNotBooksStore(testCase, database);
                notBookStore.name = 'not_books_renamed';
                notBookStore.name = 'not_books_renamed_again';

                transaction.abort();

                assert_equals(
                    notBookStore.name, 'not_books_renamed_again',
                    'IDBObjectStore.name should reflect the last rename ' +
                        'immediately after transaction.abort() returns');
                assert_array_equals(
                    database.objectStoreNames, [],
                    'IDBDatabase.objectStoreNames should not reflect the creation ' +
                        'or the rename any more immediately after transaction.abort() ' +
                        'returns');
                assert_array_equals(
                    transaction.objectStoreNames, [],
                    'IDBTransaction.objectStoreNames should not reflect the ' +
                        'creation or the rename any more immediately after ' +
                        'transaction.abort() returns');
                assert_array_equals(
                    notBookStore.indexNames, [],
                    'IDBObjectStore.indexNames for the newly created store ' +
                        'should be empty immediately after transaction.abort() ' +
                        'returns');
              }))
      .then(event => {
        assert_equals(
            notBookStore.name, 'not_books_renamed_again',
            'IDBObjectStore.name should reflect the last rename after the ' +
                'versionchange transaction is aborted');
        assert_array_equals(
            notBookStore.indexNames, [],
            'IDBObjectStore.indexNames for the newly created store ' +
                'should be empty after the versionchange transaction is aborted ' +
                'returns');
        const request = indexedDB.open(dbName, 1);
        return promiseForRequest(testCase, request);
      })
      .then(database => {
        assert_array_equals(
            database.objectStoreNames, [],
            'IDBDatabase.objectStoreNames should not reflect the creation or ' +
                'the rename after the versionchange transaction is aborted');

        database.close();
      });
}, 'IndexedDB object store creation and rename in an aborted transaction');
