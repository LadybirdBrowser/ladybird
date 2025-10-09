// META: global=window,worker
// META: title=IndexedDB: IDBObjectStore query method Ordering
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-get
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getall
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getallkeys
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-count
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-opencursor
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-openkeycursor

'use strict';

['get', 'getAll', 'getAllKeys', 'count', 'openCursor', 'openKeyCursor'].forEach(
    method => {
      indexeddb_test(
          (t, db) => {
            const store = db.createObjectStore('s');
            const store2 = db.createObjectStore('s2');

            db.deleteObjectStore('s2');

            setTimeout(
                t.step_func(() => {
                  assert_throws_dom(
                      'InvalidStateError',
                      () => {
                        store2[method]('key');
                      },
                      '"has been deleted" check (InvalidStateError) should precede ' +
                          '"not active" check (TransactionInactiveError)');

                  t.done();
                }),
                0);
          },
          (t, db) => {},
          `IDBObjectStore.${method} exception order: ` +
              'InvalidStateError vs. TransactionInactiveError');

      indexeddb_test(
          (t, db) => {
            const store = db.createObjectStore('s');
          },
          (t, db) => {
            const tx = db.transaction('s', 'readonly');
            const store = tx.objectStore('s');

            setTimeout(
                t.step_func(() => {
                  assert_throws_dom(
                      'TransactionInactiveError',
                      () => {
                        store[method]({});
                      },
                      '"not active" check (TransactionInactiveError) should precede ' +
                          'query check (DataError)');
                  t.done();
                }),
                0);
          },
          `IDBObjectStore.${method} exception order: ` +
              'TransactionInactiveError vs. DataError');
    });
