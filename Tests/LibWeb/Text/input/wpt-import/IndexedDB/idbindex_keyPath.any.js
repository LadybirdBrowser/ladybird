// META: title=IndexedDB: IDBIndex keyPath attribute
// META: script=resources/support.js
'use strict';

indexeddb_test(
  (t, db) => {
    const store = db.createObjectStore('store', {keyPath: ['a', 'b']});
    store.createIndex('index', ['a', 'b']);
  },
  (t, db) => {
    const tx = db.transaction('store', 'readonly');
    const store = tx.objectStore('store');
    const index = store.index('index');
    assert_equals(typeof index.keyPath, 'object', 'keyPath is an object');
    assert_true(Array.isArray(index.keyPath), 'keyPath is an array');

    assert_equals(
      index.keyPath, index.keyPath,
      'Same object instance is returned each time keyPath is inspected');

    const tx2 = db.transaction('store', 'readonly');
    const store2 = tx2.objectStore('store');
    const index2 = store2.index('index');

    assert_not_equals(
      index.keyPath, index2.keyPath,
      'Different instances are returned from different index instances.');

    t.done();
  },
  `IDBIndex's keyPath attribute returns the same object.`);

  indexeddb_test(
  (t, db) => {
    const store = db.createObjectStore('store', {autoIncrement: true});
    store.createIndex('index', ['a']);

    store.add({a: 1, b: 2, c: 3})
  },
  (t, db) => {
    const tx = db.transaction('store', 'readonly');
    const store = tx.objectStore('store');
    const index = store.index('index');
    const cursorReq = index.openCursor();

    cursorReq.onsuccess = t.step_func_done((e) => {
      const expectedKeyValue = [1];
      const actualKeyValue = e.target.result.key;

      assert_array_equals(actualKeyValue, expectedKeyValue, "An array keypath should yield an array key");
    });
  },
  `IDBIndex's keyPath array with a single value`);

  indexeddb_test(
  (t, db) => {
    const store = db.createObjectStore('store', {autoIncrement: true});
    store.createIndex('index', ['a', 'b']);

    store.add({a: 1, b: 2, c: 3})
  },
  (t, db) => {
    const tx = db.transaction('store', 'readonly');
    const store = tx.objectStore('store');
    const index = store.index('index');
    const cursorReq = index.openCursor();

    cursorReq.onsuccess = t.step_func_done((e) => {
      const expectedKeyValue = [1, 2];
      const actualKeyValue = e.target.result.key;

      assert_array_equals(actualKeyValue, expectedKeyValue, "An array keypath should yield an array key");
    });
  },
  `IDBIndex's keyPath array with multiple values`);
