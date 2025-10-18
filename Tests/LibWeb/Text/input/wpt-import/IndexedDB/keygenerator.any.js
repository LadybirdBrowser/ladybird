// META: global=window,worker
// META: script=resources/support.js

'use strict';

function keygenerator(objects, expected_keys, desc, func) {
    let db;
    let t = async_test("Keygenerator" + " - " + desc);
    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function(e) {
        db = e.target.result;
        let objStore = db.createObjectStore("store", { keyPath: "id", autoIncrement: true });
        for (let i = 0; i < objects.length; i++)
        {
            if (objects[i] === null)
                objStore.add({});
            else
                objStore.add({ id: objects[i] });
        }
    };

    open_rq.onsuccess = function(e) {
        let actual_keys = [];
        let rq = db.transaction("store", "readonly")
                  .objectStore("store")
                  .openCursor();
        rq.onsuccess = t.step_func(function(e) {
            let cursor = e.target.result;
            if (cursor) {
                actual_keys.push(cursor.key.valueOf());
                cursor.continue();
            }
            else {
                assert_key_equals(actual_keys, expected_keys, "keygenerator array - " + desc);
                t.done();
            }
        });
    };
}
keygenerator([null, null, null, null],  [1, 2, 3, 4],
    "starts at one, and increments by one");

keygenerator([2, null, 5, null, 6.66, 7],  [2, 3, 5, 6, 6.66, 7],
    "increments by one from last set key");

keygenerator([-10, null, "6", 6.3, [10], -2, 4, null],   [-10, -2, 1, 4, 6.3, 7, "6", [10]],
    "don't increment when new key is not bigger than current");

async_test(t => {
    let db;
    let objects = [1, null, { id: 2 }, null, 2.00001, 5, null, { id: 6 }];
    let expected = [1, 2, 2.00001, 3, 5, 6];
    let errors = 0;
    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function(e) {
        db = e.target.result;
        let objStore = db.createObjectStore("store", { keyPath: "id", autoIncrement: true });

        for (let i = 0; i < objects.length; i++)
        {
            if (objects[i] === null)
            {
                objStore.add({});
            }
            else if (typeof objects[i] === "object")
            {
                let rq = objStore.add(objects[i]);
                rq.onerror = t.step_func(function(e) {
                    errors++;
                    assert_equals(e.target.error.name, "ConstraintError");
                    assert_equals(e.type, "error");
                    e.stopPropagation();
                    e.preventDefault();
                });
                rq.onsuccess = t.step_func(function(e) {
                    assert_unreached("Got rq.success when adding duplicate id " + objects[i]);
                });
            }
            else
                objStore.add({ id: objects[i] });
        }
    };

    open_rq.onsuccess = function(e) {
        let actual_keys = [];
        let rq = db.transaction("store", "readonly")
                 .objectStore("store")
                 .openCursor();
        rq.onsuccess = t.step_func(function(e) {
            let cursor = e.target.result;
            if (cursor) {
                actual_keys.push(cursor.key.valueOf());
                cursor.continue();
            }
            else {
                assert_equals(errors, 2, "expected ConstraintError's");
                assert_array_equals(actual_keys, expected, "keygenerator array");
                t.done();
            }
        });
    };

}, "Keygenerator ConstraintError when using same id as already generated");

function big_key_test(key, description) {
  indexeddb_test(
    (t, db) => {
      assert_equals(indexedDB.cmp(key, key), 0, 'Key is valid');
      db.createObjectStore('store', {autoIncrement: true});
    },
    (t, db) => {
      const tx = db.transaction('store', 'readwrite');
      const store = tx.objectStore('store');
      const value = 0;
      let request;
      request = store.put(value);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, 1,
                      'Key generator should initially be 1');
      });

      request = store.put(value);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, 2,
                      'Key generator should increment');
      });

      request = store.put(value, 1000);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, 1000,
                      'Explicit key should be used');
      });

      request = store.put(value);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, 1001,
                      'Key generator should have updated');
      });

      request = store.put(value, key);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, key,
                      'Explicit key should be used');
      });

      if (key >= 0) {
        // Large positive values will max out the key generator, so it
        // can no longer produce keys.
        request = store.put(value);
        request.onsuccess = t.unreached_func('put should fail');
        request.onerror = t.step_func(e => {
          e.preventDefault();
          assert_equals(e.target.error.name, 'ConstraintError',
                        'Key generator should have returned failure');
        });
      } else {
        // Large negative values are always lower than the key generator's
        // current number, so have no effect on the generator.
        request = store.put(value);
        request.onerror = t.unreached_func('put should succeed');
        request.onsuccess = t.step_func(e => {
          assert_equals(e.target.result, 1002,
                        'Key generator should have updated');
        });
      }

      request = store.put(value, 2000);
      request.onerror = t.unreached_func('put should succeed');
      request.onsuccess = t.step_func(e => {
        assert_equals(e.target.result, 2000,
                      'Explicit key should be used');
      });
      tx.onabort = t.step_func(() => {
        assert_unreached(`Transaction aborted: ${tx.error.message}`);
      });
      tx.oncomplete = t.step_func(() => { t.done(); });
    },
    description);
}

[
  {
    key: Number.MAX_SAFE_INTEGER + 1,
    description: '53 bits'
  },
  {
    key: Math.pow(2, 60),
    description: 'greater than 53 bits, less than 64 bits'
  },
  {
    key: -Math.pow(2, 60),
    description: 'greater than 53 bits, less than 64 bits (negative)'
  },
  {
    key: Math.pow(2, 63),
    description: '63 bits'
  },
  {
    key: -Math.pow(2, 63),
    description: '63 bits (negative)'
  },
  {
    key: Math.pow(2, 64),
    description: '64 bits'
  },
  {
    key: -Math.pow(2, 64),
    description: '64 bits (negative)'
  },
  {
    key: Math.pow(2, 70),
    description: 'greater than 64 bits, but still finite'
  },
  {
    key: -Math.pow(2, 70),
    description: 'greater than 64 bits, but still finite (negative)'
  },
  {
    key: Infinity,
    description: 'equal to Infinity'
  },
  {
    key: -Infinity,
    description: 'equal to -Infinity'
  }
].forEach(function(testCase) {
  big_key_test(testCase.key,
               `Key generator vs. explicit key ${testCase.description}`);
});

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {autoIncrement: true, keyPath: 'id'});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readwrite');
    t.onabort = t.unreached_func('transaction should not abort');
    const store = tx.objectStore('store');
    store.put({name: 'n'}).onsuccess = t.step_func(e => {
      const key = e.target.result;
      assert_equals(key, 1, 'Key generator initial value should be 1');
      store.get(key).onsuccess = t.step_func(e => {
        const value = e.target.result;
        assert_equals(typeof value, 'object', 'Result should be object');
        assert_equals(value.name, 'n', 'Result should have name property');
        assert_equals(value.id, key, 'Key should be injected');
        t.done();
      });
    });
  },
  'Key is injected into value - single segment path');

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {autoIncrement: true, keyPath: 'a.b.id'});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readwrite');
    t.onabort = t.unreached_func('transaction should not abort');
    const store = tx.objectStore('store');
    store.put({name: 'n'}).onsuccess = t.step_func(e => {
      const key = e.target.result;
      assert_equals(key, 1, 'Key generator initial value should be 1');
      store.get(key).onsuccess = t.step_func(e => {
        const value = e.target.result;
        assert_equals(typeof value, 'object', 'Result should be object');
        assert_equals(value.name, 'n', 'Result should have name property');
        assert_equals(value.a.b.id, key, 'Key should be injected');
        t.done();
      });
    });
  },
  'Key is injected into value - multi-segment path');

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {autoIncrement: true, keyPath: 'a.b.id'});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readwrite');
    t.onabort = t.unreached_func('transaction should not abort');
    const store = tx.objectStore('store');
    store.put({name: 'n1', b: {name: 'n2'}}).onsuccess = t.step_func(e => {
      const key = e.target.result;
      assert_equals(key, 1, 'Key generator initial value should be 1');
      store.get(key).onsuccess = t.step_func(e => {
        const value = e.target.result;
        assert_equals(typeof value, 'object', 'Result should be object');
        assert_equals(value.name, 'n1', 'Result should have name property');
        assert_equals(value.b.name, 'n2', 'Result should have name property');
        assert_equals(value.a.b.id, key, 'Key should be injected');
        t.done();
      });
    });
  },
  'Key is injected into value - multi-segment path, partially populated');

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {autoIncrement: true, keyPath: 'id'});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readwrite');
    const store = tx.objectStore('store');

    assert_throws_dom('DataError', () => {
      store.put(123);
    }, 'Key path should be checked against value');

    t.done();
  },
  'put() throws if key cannot be injected - single segment path');

indexeddb_test(
  (t, db) => {
    db.createObjectStore('store', {autoIncrement: true, keyPath: 'a.b.id'});
  },
  (t, db) => {
    const tx = db.transaction('store', 'readwrite');
    const store = tx.objectStore('store');

    assert_throws_dom('DataError', () => {
      store.put({a: 123});
    }, 'Key path should be checked against value');

    assert_throws_dom('DataError', () => {
      store.put({a: {b: 123} });
    }, 'Key path should be checked against value');

    t.done();
  },
  'put() throws if key cannot be injected - multi-segment path');

async_test(t => {
    let db;
    let overflow_error_fired = false;
    let objects = [9007199254740991, null, "error", 2, "error"];
    let expected_keys = [2, 9007199254740991, 9007199254740992];
    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function(e) {
        db = e.target.result;
        let objStore = db.createObjectStore("store", { keyPath: "id", autoIncrement: true });
        for (let i = 0; i < objects.length; i++)
        {
            if (objects[i] === null)
            {
                objStore.add({});
            }
            else if (objects[i] === "error")
            {
                let rq = objStore.add({});
                rq.onsuccess = fail(t, 'When "current number" overflows, error event is expected');
                rq.onerror = t.step_func(function(e) {
                    overflow_error_fired = true;
                    assert_equals(e.target.error.name, "ConstraintError", "error name");
                    e.preventDefault();
                    e.stopPropagation();
                });
            }
            else
                objStore.add({ id: objects[i] });
        }
    };

    open_rq.onsuccess = function(e) {
        let actual_keys = [];
        let rq = db.transaction("store", "readonly")
                 .objectStore("store")
                 .openCursor();
        rq.onsuccess = t.step_func(function(e) {
            let cursor = e.target.result;
            if (cursor) {
                actual_keys.push(cursor.key.valueOf());
                cursor.continue();
            }
            else {
                assert_true(overflow_error_fired, "error fired on 'current number' overflow");
                assert_array_equals(actual_keys, expected_keys, "keygenerator array");

                t.done();
            }
        });
    };
}, "Keygenerator overflow");
