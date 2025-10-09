// META: title=IndexedDB: Test IDBObjectStore.getKey()
// META: script=resources/support.js

'use strict';

function getkey_test(func, name) {
  indexeddb_test(
    (t, db, tx) => {
      const basic = db.createObjectStore('basic');
      const key_path_store = db.createObjectStore('key path',
        { keyPath: 'id' });
      const key_generator_store = db.createObjectStore('key generator',
        { autoIncrement: true });
      const key_generator_and_path_store = db.createObjectStore(
        'key generator and key path',
        { autoIncrement: true, key_path: 'id' });

      for (let i = 1; i <= 10; ++i) {
        basic.put(`value: ${i}`, i);
        key_path_store.put({ id: i });
        key_generator_store.put(`value: ${i}`);
        key_generator_and_path_store.put({});
      }
    },
    func,
    name
  );
}

getkey_test((t, db) => {
  const tx = db.transaction('basic', 'readonly');
  const store = tx.objectStore('basic');
  assert_throws_js(TypeError, () => store.getKey());
  assert_throws_dom('DataError', () => store.getKey(null));
  assert_throws_dom('DataError', () => store.getKey({}));
  t.done();
}, 'IDBObjectStore.getKey() - invalid parameters');

[
  'basic',
  'key path',
  'key generator',
  'key generator and key path'
].forEach(store_name => {
  getkey_test((t, db) => {
    const tx = db.transaction(store_name);
    const store = tx.objectStore(store_name);
    const request = store.getKey(5);
    request.onerror = t.unreached_func('request failed');
    request.onsuccess = t.step_func(() =>
      assert_equals(request.result, 5));
    tx.onabort = t.unreached_func('transaction aborted');
    tx.oncomplete = t.step_func(() => t.done());
  }, `IDBObjectStore.getKey() - ${store_name} - key`);

  getkey_test((t, db) => {
    const tx = db.transaction(store_name);
    const store = tx.objectStore(store_name);
    const request = store.getKey(IDBKeyRange.lowerBound(4.5));
    request.onerror = t.unreached_func('request failed');
    request.onsuccess = t.step_func(() =>
      assert_equals(request.result, 5));
    tx.onabort = t.unreached_func('transaction aborted');
    tx.oncomplete = t.step_func(() => t.done());
  }, `IDBObjectStore.getKey() - ${store_name} - range`);

  getkey_test((t, db) => {
    const tx = db.transaction(store_name);
    const store = tx.objectStore(store_name);
    const request = store.getKey(11);
    request.onerror = t.unreached_func('request failed');
    request.onsuccess = t.step_func(() =>
      assert_equals(request.result, undefined));
    tx.onabort = t.unreached_func('transaction aborted');
    tx.oncomplete = t.step_func(() => t.done());
  }, `IDBObjectStore.getKey() - ${store_name} - key - no match`);

  getkey_test((t, db) => {
    const tx = db.transaction(store_name);
    const store = tx.objectStore(store_name);
    const request = store.getKey(IDBKeyRange.lowerBound(11));
    request.onerror = t.unreached_func('request failed');
    request.onsuccess = t.step_func(() =>
      assert_equals(request.result, undefined)
    );
    tx.onabort = t.unreached_func('transaction aborted');
    tx.oncomplete = t.step_func(() => t.done());
  }, `IDBObjectStore.getKey() - ${store_name} - range - no match`);
});
