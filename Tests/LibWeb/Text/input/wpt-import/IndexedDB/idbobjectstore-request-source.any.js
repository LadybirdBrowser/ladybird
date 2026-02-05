// META: global=window,worker
// META: title=IndexedDB: The source of requests made against object stores
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbrequest-source

'use strict';

[
    store => store.put(0),
    store => store.add(0),
    store => store.delete(0),
    store => store.clear(),

    store => store.get(0),
    store => store.getKey(0),
    store => store.getAll(),
    store => store.getAllKeys(),
    store => store.count(),

    store => store.openCursor(),
    store => store.openKeyCursor()

].forEach(
        func => indexeddb_test(
            (t, db) => {
              db.createObjectStore('store', {autoIncrement: true});
            },
            (t, db) => {
              const tx = db.transaction('store', 'readwrite');
              const store = tx.objectStore('store');

              assert_equals(
                  func(store).source, store,
                  `${func}.source should be the object store itself`);
              t.done();
            },
            `The source of the request from ${
                func} is the object store itself`));
