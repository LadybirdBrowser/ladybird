// META: global=window,worker
// META: title=IndexedDB: IDBObjectStore add()/put() Exception Ordering
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-put
// Spec: https://w3c.github.io/IndexedDB/#dom-idbobjectstore-add

'use strict';

['put', 'add'].forEach(method => {
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
                    store2[method]('key', 'value');
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
                    store[method]('key', 'value');
                  },
                  '"not active" check (TransactionInactiveError) should precede ' +
                      '"read only" check (ReadOnlyError)');
              t.done();
            }),
            0);
      },

      `IDBObjectStore.${method} exception order: ` +
          'TransactionInactiveError vs. ReadOnlyError');

  indexeddb_test(
      (t, db) => {
        const store = db.createObjectStore('s');
      },
      (t, db) => {
        const tx = db.transaction('s', 'readonly');
        const store = tx.objectStore('s');

        assert_throws_dom(
            'ReadOnlyError',
            () => {
              store[method]({}, 'value');
            },
            '"read only" check (ReadOnlyError) should precede ' +
                'key/data check (DataError)');

        t.done();
      },

      `IDBObjectStore.${method} exception order: ` +
          'ReadOnlyError vs. DataError');
});
