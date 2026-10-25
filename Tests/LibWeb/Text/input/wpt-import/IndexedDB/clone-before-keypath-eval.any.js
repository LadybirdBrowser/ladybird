// META: title=IndexedDB: clone before key path evaluation
// META: global=window,worker
// META: script=resources/support.js

'use strict';

function ProbeObject() {
  this.id_count = 0;
  this.invalid_id_count = 0;
  this.prop_count = 0;
  Object.defineProperties(this, {
    id: {
      enumerable: true,
      get() {
        ++this.id_count;
        return 1000 + this.id_count;
      },
    },
    invalid_id: {
      enumerable: true,
      get() {
        ++this.invalid_id_count;
        return {};
      },
    },
    prop: {
      enumerable: true,
      get() {
        ++this.prop_count;
        return 2000 + this.prop_count;
      },
    },
  });
}

function createObjectStoreWithKeyPath(
    storeName, keyPath, autoIncrement = false) {
  return (t, db) => {
    db.createObjectStore(storeName, {keyPath, autoIncrement});
  };
}

function createObjectStoreWithIndex(
    storeName, keyPath, indexName, indexKeyPath) {
  return (t, db) => {
    const storeOptions = keyPath ? {keyPath} : {};
    const store = db.createObjectStore(storeName, storeOptions);

    // If index parameters are provided, create the index.
    if (indexName && indexKeyPath) {
      store.createIndex(indexName, indexKeyPath);
    }
  };
}

function createTransactionAndReturnObjectStore(db, storeName) {
  const tx = db.transaction(storeName, 'readwrite');
  const store = tx.objectStore(storeName);
  return {tx, store};
}

indexeddb_test(createObjectStoreWithKeyPath('store', 'id', true), (t, db) => {
  const {store} = createTransactionAndReturnObjectStore(db, 'store');
  const obj = new ProbeObject();
  store.put(obj);
  assert_equals(
      obj.id_count, 1,
      'put() operation should access primary key property once');
  assert_equals(
      obj.prop_count, 1, 'put() operation should access other properties once');
  t.done();
}, 'Key generator and key path validity check operates on a clone');

indexeddb_test(
    createObjectStoreWithKeyPath('store', 'invalid_id', true), (t, db) => {
      const {store} = createTransactionAndReturnObjectStore(db, 'store');
      const obj = new ProbeObject();
      assert_throws_dom('DataError', () => {
        store.put(obj);
      }, 'put() should throw if primary key cannot be injected');
      assert_equals(
          obj.invalid_id_count, 1,
          'put() operation should access primary key property once');
      assert_equals(
          obj.prop_count, 1,
          'put() operation should access other properties once');
      t.done();
    }, 'Failing key path validity check operates on a clone');

indexeddb_test(
    createObjectStoreWithIndex('store', null, 'index', 'prop'), (t, db) => {
      const {store} = createTransactionAndReturnObjectStore(db, 'store');
      const obj = new ProbeObject();
      store.put(obj, 'key');
      assert_equals(
          obj.prop_count, 1, 'put() should access index key property once');
      assert_equals(
          obj.id_count, 1,
          'put() operation should access other properties once');
      t.done();
    }, 'Index key path evaluations operate on a clone');

indexeddb_test(
    createObjectStoreWithIndex('store', 'id', 'index', 'prop'), (t, db) => {
      const {store} = createTransactionAndReturnObjectStore(db, 'store');
      const obj = new ProbeObject();
      store.put(obj);
      assert_equals(
          obj.id_count, 1, 'put() should access primary key property once');
      assert_equals(
          obj.prop_count, 1, 'put() should access index key property once');
      t.done();
    }, 'Store and index key path evaluations operate on the same clone');

indexeddb_test(
    createObjectStoreWithIndex('store', 'id', 'index', 'prop'), (t, db) => {
      const {store} = createTransactionAndReturnObjectStore(db, 'store');
      store.put(new ProbeObject());

      store.openCursor().onsuccess = t.step_func((event) => {
        const cursor = event.target.result;

        const obj = new ProbeObject();
        cursor.update(obj);
        assert_equals(
            obj.id_count, 1, 'put() should access primary key property once');
        assert_equals(
            obj.prop_count, 1, 'put() should access index key property once');

        t.done();
      });
    }, 'Cursor update checks and keypath evaluations operate on a clone');

indexeddb_test(
    createObjectStoreWithIndex(
        'store', null, 'index', 'blob.expando.val'),
    (t, db) => {
      const {store} = createTransactionAndReturnObjectStore(db, 'store');

      const blob = new Blob(['data']);
      let trapFired = false;
      blob.expando = new Proxy({val: 'key'}, {
        getOwnPropertyDescriptor(target, prop) {
          trapFired = true;
          return Reflect.getOwnPropertyDescriptor(target, prop);
        },
      });

      const request = store.put({blob}, 'key');
      request.onsuccess = t.step_func(() => {
        assert_false(trapFired, 'Proxy trap on Blob expando must not fire');
        t.done();
      });
      request.onerror = t.unreached_func('put() should not fail');
    },
    'Blob expando properties are stripped in the clone used for index key path evaluation');
