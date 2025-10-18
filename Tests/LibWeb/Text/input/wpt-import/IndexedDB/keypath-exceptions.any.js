// META: global=window,worker
// META: title=IndexedDB: Exceptions in extracting keys from values (ES bindings)
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#extract-key-from-value

'use strict';

indexeddb_test(
    (t, db) => {
      db.createObjectStore('store', {autoIncrement: true, keyPath: 'a.b.c'});
    },
    (t, db) => {
      const tx = db.transaction('store', 'readwrite', {durability: 'relaxed'});
      assert_throws_dom('DataError', () => {
        tx.objectStore('store').put({a: {b: 'foo'}});
      }, 'Put should throw if key can not be inserted at key path location.');
      t.done();
    },
    'The last element of keypath is validated');

const err = Error();
err.name = 'getter';

function throwingGetter() {
  throw err;
}

indexeddb_test(
    function(t, db) {
      const o = {};
      Object.defineProperty(
          o, 'throws',
          {get: throwingGetter, enumerable: false, configurable: true});

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no such property, so key path evaluation
      // will fail.
      const s1 = db.createObjectStore('s1', {keyPath: 'throws'});
      assert_throws_dom('DataError', () => {
        s1.put(o);
      }, 'Key path failing to resolve should throw');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no such property, so key path evaluation
      // will fail.
      const s2 = db.createObjectStore('s2', {keyPath: 'throws.x'});
      assert_throws_dom('DataError', () => {
        s2.put(o);
      }, 'Key path failing to resolve should throw');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no such property, so generated key can be
      // inserted.
      const s3 =
          db.createObjectStore('s3', {keyPath: 'throws', autoIncrement: true});
      assert_class_string(
          s3.put(o), 'IDBRequest',
          'Key injectability test at throwing getter should succeed');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no such property, so intermediate object
      // and generated key can be inserted.
      const s4 = db.createObjectStore(
          's4', {keyPath: 'throws.x', autoIncrement: true});
      assert_class_string(
          s4.put(o), 'IDBRequest',
          'Key injectability test past throwing getter should succeed');
    },
    (t, db) => {
      t.done();
    },
    'Key path evaluation: Exceptions from non-enumerable getters');

indexeddb_test(
    function(t, db) {
      const o = {};
      Object.defineProperty(
          o, 'throws',
          {get: throwingGetter, enumerable: true, configurable: true});

      // Value should be cloned before key path is evaluated,
      // and enumerable getter will rethrow.
      const s1 = db.createObjectStore('s1', {keyPath: 'throws'});
      assert_throws_exactly(err, () => {
        s1.put(o);
      }, 'Key path resolving to throwing getter rethrows');

      // Value should be cloned before key path is evaluated,
      // and enumerable getter will rethrow.
      const s2 = db.createObjectStore('s2', {keyPath: 'throws.x'});
      assert_throws_exactly(err, () => {
        s2.put(o);
      }, 'Key path resolving past throwing getter rethrows');

      // Value should be cloned before key path is evaluated,
      // and enumerable getter will rethrow.
      const s3 =
          db.createObjectStore('s3', {keyPath: 'throws', autoIncrement: true});
      assert_throws_exactly(err, () => {
        s3.put(o);
      }, 'Key injectability test at throwing getter should rethrow');

      // Value should be cloned before key path is evaluated,
      // and enumerable getter will rethrow.
      const s4 = db.createObjectStore(
          's4', {keyPath: 'throws.x', autoIncrement: true});
      assert_throws_exactly(err, () => {
        s4.put(o);
      }, 'Key injectability test past throwing getter should rethrow');
    },
    (t, db) => {
      t.done();
    },
    'Key path evaluation: Exceptions from enumerable getters');

indexeddb_test(
    (t, db) => {
      // Implemented as function wrapper to clean up
      // immediately after use, otherwise it may
      // interfere with the test harness.
      function with_proto_getter(f) {
        return function() {
          Object.defineProperty(
              Object.prototype, 'throws',
              {get: throwingGetter, enumerable: false, configurable: true});
          try {
            f();
          } finally {
            delete Object.prototype['throws'];
          }
        };
      }

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no own property, so key path evaluation will
      // fail and DataError should be thrown.
      const s1 = db.createObjectStore('s1', {keyPath: 'throws'});
      assert_throws_dom(
          'DataError', with_proto_getter(function() {
            s1.put({});
          }),
          'Key path resolving to no own property throws DataError');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no own property, so key path evaluation will
      // fail and DataError should be thrown.
      const s2 = db.createObjectStore('s2', {keyPath: 'throws.x'});
      assert_throws_dom(
          'DataError', with_proto_getter(function() {
            s2.put({});
          }),
          'Key path resolving past no own property throws DataError');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no own property, so key path evaluation will
      // fail and injection can succeed.
      const s3 =
          db.createObjectStore('s3', {keyPath: 'throws', autoIncrement: true});
      assert_equals(
          s3.put({}).readyState, 'pending',
          'put should not throw due to inherited property');

      // Value should be cloned before key path is evaluated,
      // and non-enumerable getter will be ignored. The clone
      // will have no own property, so key path evaluation will
      // fail and injection can succeed.
      const s4 = db.createObjectStore(
          's4', {keyPath: 'throws.x', autoIncrement: true});
      assert_equals(
          s4.put({}).readyState, 'pending',
          'put should not throw due to inherited property');
    },
    (t, db) => {
      t.done();
    },
    'Key path evaluation: Exceptions from non-enumerable getters on prototype');

indexeddb_test(
    (t, db) => {
      // Implemented as function wrapper to clean up
      // immediately after use, otherwise it may
      // interfere with the test harness.
      function with_proto_getter(f) {
        return () => {
          Object.defineProperty(
              Object.prototype, 'throws',
              {get: throwingGetter, enumerable: true, configurable: true});
          try {
            f();
          } finally {
            delete Object.prototype['throws'];
          }
        };
      }

      // Value should be cloned before key path is evaluated.
      // The clone will have no own property, so key path
      // evaluation will fail and DataError should be thrown.
      const s1 = db.createObjectStore('s1', {keyPath: 'throws'});
      assert_throws_dom(
          'DataError', with_proto_getter(function() {
            s1.put({});
          }),
          'Key path resolving to no own property throws DataError');

      // Value should be cloned before key path is evaluated.
      // The clone will have no own property, so key path
      // evaluation will fail and DataError should be thrown.
      const s2 = db.createObjectStore('s2', {keyPath: 'throws.x'});
      assert_throws_dom(
          'DataError', with_proto_getter(function() {
            s2.put({});
          }),
          'Key path resolving past throwing getter rethrows');

      // Value should be cloned before key path is evaluated.
      // The clone will have no own property, so key path
      // evaluation will fail and injection can succeed.
      let s3 =
          db.createObjectStore('s3', {keyPath: 'throws', autoIncrement: true});
      assert_equals(
          s3.put({}).readyState, 'pending',
          'put should not throw due to inherited property');

      // Value should be cloned before key path is evaluated.
      // The clone will have no own property, so key path
      // evaluation will fail and injection can succeed.
      let s4 = db.createObjectStore(
          's4', {keyPath: 'throws.x', autoIncrement: true});
      assert_equals(
          s4.put({}).readyState, 'pending',
          'put should not throw due to inherited property');
    },
    (t, db) => {
      t.done();
    },
    'Key path evaluation: Exceptions from enumerable getters on prototype');

indexeddb_test(
    (t, db) => {
      const store = db.createObjectStore('store');
      store.createIndex('index', 'index0');
    },
    (t, db) => {
      const tx = db.transaction('store', 'readwrite', {durability: 'relaxed'});

      const array = [];
      array[99] = 1;

      // Implemented as function wrapper to clean up
      // immediately after use, otherwise it may
      // interfere with the test harness.
      let getter_called = 0;
      function with_proto_getter(f) {
        const prop = '50';
        Object.defineProperty(Object.prototype, prop, {
          enumerable: true,
          configurable: true,
          get: () => {
            ++getter_called;
            return 'foo';
          }
        });
        try {
          return f();
        } finally {
          delete Object.prototype[prop];
        }
      }

      const request = with_proto_getter(
          () => tx.objectStore('store').put({index0: array}, 'key'));
      request.onerror = t.unreached_func('put should not fail');
      request.onsuccess = t.step_func(function() {
        assert_equals(
            getter_called, 0, 'Prototype getter should not be called');
        t.done();
      });
    },
    'Array key conversion should not invoke prototype getters');
