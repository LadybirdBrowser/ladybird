// META: title=IndexedDB: IDBIndex query method Ordering
// META: global=window,worker
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-get
// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-getall
// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-getallkeys
// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-count
// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-opencursor
// Spec: https://w3c.github.io/IndexedDB/#dom-idbindex-openkeycursor

'use strict';

[   'get',
    'getAll',
    'getAllKeys',
    'count',
    'openCursor',
    'openKeyCursor'
   ].forEach(method => {
     indexeddb_test(
       (t, db) => {
         const store = db.createObjectStore('s');
         const store2 = db.createObjectStore('s2');
         const index = store2.createIndex('i', 'keyPath');
         store2.deleteIndex('i');

         setTimeout(t.step_func(() => {
           assert_throws_dom(
             'InvalidStateError', () => { index[method]('key'); },
             '"has been deleted" check (InvalidStateError) should precede ' +
             '"not active" check (TransactionInactiveError)');
           t.done();
         }), 0);
       },
       (t, db) => {},
       `IDBIndex.${method} exception order: ` +
       'InvalidStateError vs. TransactionInactiveError'
     );

     indexeddb_test(
       (t, db) => {
         const store = db.createObjectStore('s');
         const index = store.createIndex('i', 'keyPath');
       },
       (t, db) => {
         const tx = db.transaction('s', 'readonly');
         const store = tx.objectStore('s');
         const index = store.index('i');

         setTimeout(t.step_func(() => {
           assert_throws_dom(
             'TransactionInactiveError', () => { index[method]({}); },
             '"not active" check (TransactionInactiveError) should precede ' +
             'query check (DataError)');
           t.done();
         }), 0);
       },
       `IDBIndex.${method} exception order: ` +
       'TransactionInactiveError vs. DataError'
     );
   });
