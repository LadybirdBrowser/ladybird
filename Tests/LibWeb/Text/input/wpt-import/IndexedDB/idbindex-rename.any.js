// META: title=IndexedDB: index renaming support
// META: global=window,worker
// META: script=resources/support-promises.js
// META: timeout=long

// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-name

'use strict';

promise_test(testCase => {
  let authorIndex = null;
  let authorIndex2 = null;
  let renamedAuthorIndex = null;
  let renamedAuthorIndex2 = null;
  return createDatabase(
             testCase,
             (database, transaction) => {
               const store = createBooksStore(testCase, database);
               authorIndex = store.index('by_author');
             })
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_author', 'by_title'],
            'Test setup should have created two indexes');
        authorIndex2 = store.index('by_author');
        return checkAuthorIndexContents(
                   testCase, authorIndex2,
                   'The index should have the expected contents before any renaming')
            .then(() => database.close());
      })
      .then(
          () => migrateDatabase(
              testCase, 2,
              (database, transaction) => {
                const store = transaction.objectStore('books');
                renamedAuthorIndex = store.index('by_author');
                renamedAuthorIndex.name = 'renamed_by_author';

                assert_equals(
                    renamedAuthorIndex.name, 'renamed_by_author',
                    'IDBIndex name should change immediately after a rename');
                assert_array_equals(
                    store.indexNames, ['by_title', 'renamed_by_author'],
                    'IDBObjectStore.indexNames should immediately reflect the rename');
                assert_equals(
                    store.index('renamed_by_author'), renamedAuthorIndex,
                    'IDBObjectStore.index should return the renamed index store when ' +
                        'queried using the new name immediately after the rename');
                assert_throws_dom(
                    'NotFoundError', () => store.index('by_author'),
                    'IDBObjectStore.index should throw when queried using the ' +
                        'renamed index\'s old name immediately after the rename');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_title', 'renamed_by_author'],
            'IDBObjectStore.indexNames should still reflect the rename after ' +
                'the versionchange transaction commits');
        renamedAuthorIndex2 = store.index('renamed_by_author');
        return checkAuthorIndexContents(
                   testCase, renamedAuthorIndex2,
                   'Renaming an index should not change its contents')
            .then(() => database.close());
      })
      .then(() => {
        assert_equals(
            authorIndex.name, 'by_author',
            'IDBIndex obtained before the rename transaction should not ' +
                'reflect the rename');
        assert_equals(
            authorIndex2.name, 'by_author',
            'IDBIndex obtained before the rename transaction should not ' +
                'reflect the rename');
        assert_equals(
            renamedAuthorIndex.name, 'renamed_by_author',
            'IDBIndex used in the rename transaction should keep reflecting ' +
                'the new name after the transaction is committed');
        assert_equals(
            renamedAuthorIndex2.name, 'renamed_by_author',
            'IDBIndex obtained after the rename transaction should reflect ' +
                'the new name');
      });
}, 'IndexedDB index rename in new transaction');

promise_test(testCase => {
  let renamedAuthorIndex = null;
  let renamedAuthorIndex2 = null;
  return createDatabase(
             testCase,
             (database, transaction) => {
               const store = createBooksStore(testCase, database);
               renamedAuthorIndex = store.index('by_author');
               renamedAuthorIndex.name = 'renamed_by_author';

               assert_equals(
                   renamedAuthorIndex.name, 'renamed_by_author',
                   'IDBIndex name should change immediately after a rename');
               assert_array_equals(
                   store.indexNames, ['by_title', 'renamed_by_author'],
                   'IDBObjectStore.indexNames should immediately reflect the rename');
               assert_equals(
                   store.index('renamed_by_author'), renamedAuthorIndex,
                   'IDBObjectStore.index should return the renamed index store when ' +
                       'queried using the new name immediately after the rename');
               assert_throws_dom(
                   'NotFoundError', () => store.index('by_author'),
                   'IDBObjectStore.index should throw when queried using the ' +
                       'renamed index\'s old name immediately after the rename');
             })
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_title', 'renamed_by_author'],
            'IDBObjectStore.indexNames should still reflect the rename after ' +
                'the versionchange transaction commits');
        renamedAuthorIndex2 = store.index('renamed_by_author');
        return checkAuthorIndexContents(
                   testCase, renamedAuthorIndex2,
                   'Renaming an index should not change its contents')
            .then(() => database.close());
      })
      .then(() => {
        assert_equals(
            renamedAuthorIndex.name, 'renamed_by_author',
            'IDBIndex used in the rename transaction should keep reflecting ' +
                'the new name after the transaction is committed');
        assert_equals(
            renamedAuthorIndex2.name, 'renamed_by_author',
            'IDBIndex obtained after the rename transaction should reflect ' +
                'the new name');
      });
}, 'IndexedDB index rename in the transaction where it is created');

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
                index.name = 'by_author';
                assert_array_equals(
                    store.indexNames, ['by_author', 'by_title'],
                    'Renaming an index to the same name should not change the ' +
                        'index\'s IDBObjectStore.indexNames');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_author', 'by_title'],
            'Committing a transaction that renames a store to the same name ' +
                'should not change the index\'s IDBObjectStore.indexNames');
        const index = store.index('by_author');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Committing a transaction that renames an index to the same name ' +
                       'should not change the index\'s contents')
            .then(() => database.close());
      });
}, 'IndexedDB index rename to the same name succeeds');

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
                store.deleteIndex('by_title');
                index.name = 'by_title';
                assert_array_equals(
                    store.indexNames, ['by_title'],
                    'IDBObjectStore.indexNames should immediately reflect the rename');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_title'],
            'IDBObjectStore.indexNames should still reflect the rename after ' +
                'the versionchange transaction commits');
        const index = store.index('by_title');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Renaming an index should not change its contents')
            .then(() => database.close());
      });
}, 'IndexedDB index rename to the name of a deleted index succeeds');

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
                store.index('by_author').name = 'tmp';
                store.index('by_title').name = 'by_author';
                store.index('tmp').name = 'by_title';
                assert_array_equals(
                    store.indexNames, ['by_author', 'by_title'],
                    'IDBObjectStore.indexNames should reflect the swap immediately ' +
                        'after the renames');
                return checkTitleIndexContents(
                    testCase, store.index('by_author'),
                    'Renaming an index should not change its contents');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_author', 'by_title'],
            'IDBObjectStore.indexNames should still reflect the swap after ' +
                'the versionchange transaction commits');
        const index = store.index('by_title');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Renaming an index should not change its contents')
            .then(() => database.close());
      });
}, 'IndexedDB index swapping via renames succeeds');

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

                index.name = 42;
                assert_equals(
                    index.name, '42',
                    'IDBIndex name should change immediately after a rename to a ' +
                        'number');
                assert_array_equals(
                    store.indexNames, ['42', 'by_title'],
                    'IDBObjectStore.indexNames should immediately reflect the ' +
                        'stringifying rename');

                index.name = true;
                assert_equals(
                    index.name, 'true',
                    'IDBIndex name should change immediately after a rename to a ' +
                        'boolean');

                index.name = {};
                assert_equals(
                    index.name, '[object Object]',
                    'IDBIndex name should change immediately after a rename to an ' +
                        'object');

                index.name = () => null;
                assert_equals(
                    index.name, '() => null',
                    'IDBIndex name should change immediately after a rename to a ' +
                        'function');

                index.name = undefined;
                assert_equals(
                    index.name, 'undefined',
                    'IDBIndex name should change immediately after a rename to ' +
                        'undefined');
              }))
      .then(database => {
        const transaction = database.transaction('books', 'readonly');
        const store = transaction.objectStore('books');
        assert_array_equals(
            store.indexNames, ['by_title', 'undefined'],
            'IDBObjectStore.indexNames should reflect the last rename ' +
                'after the versionchange transaction commits');
        const index = store.index('undefined');
        return checkAuthorIndexContents(
                   testCase, index,
                   'Renaming an index should not change its contents')
            .then(() => database.close());
      });
}, 'IndexedDB index rename stringifies non-string names');

for (let escapedName of ['', '\\u0000', '\\uDC00\\uD800'])
  ((escapedName) => {
    const name = JSON.parse('"' + escapedName + '"');
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

                    index.name = name;
                    assert_equals(
                        index.name, name,
                        'IDBIndex name should change immediately after the rename');
                    assert_array_equals(
                        store.indexNames, [name, 'by_title'].sort(),
                        'IDBObjectStore.indexNames should immediately reflect the rename');
                  }))
          .then(database => {
            const transaction = database.transaction('books', 'readonly');
            const store = transaction.objectStore('books');
            assert_array_equals(
                store.indexNames, [name, 'by_title'].sort(),
                'IDBObjectStore.indexNames should reflect the rename ' +
                    'after the versionchange transaction commits');
            const index = store.index(name);
            return checkAuthorIndexContents(
                       testCase, index,
                       'Renaming an index should not change its contents')
                .then(() => database.close());
          });
    }, 'IndexedDB index can be renamed to "' + escapedName + '"');
  })(escapedName);
