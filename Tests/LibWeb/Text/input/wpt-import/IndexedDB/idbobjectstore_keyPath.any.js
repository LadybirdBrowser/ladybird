// META: title=IndexedDB: IDBObjectStore keyPath attribute - same object
// META: script=resources/support.js
'use strict';

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {keyPath: ['a', 'b']});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readonly');
    const store = tx.objectStore('store');
    assert_equals(typeof store.keyPath, 'object', 'keyPath is an object');
    assert_true(Array.isArray(store.keyPath), 'keyPath is an array');

    assert_equals(
      store.keyPath, store.keyPath,
      'Same object instance is returned each time keyPath is inspected');

    const tx2 = db.transaction('store', 'readonly');
    const store2 = tx2.objectStore('store');

    assert_not_equals(
      store.keyPath, store2.keyPath,
      'Different instances are returned from different store instances.');

    t.done();
  },
  `IDBObjectStore's keyPath attribute returns the same object.`);
