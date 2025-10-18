// META: title=IndexedDB: Binary keys written to a database and read back
// META: global=window,worker
// META: timeout=long
// META: script=resources/support.js

'use strict';

const sample = [0x44, 0x33, 0x22, 0x11, 0xFF, 0xEE, 0xDD, 0xCC];
const buffer = new Uint8Array(sample).buffer;

function assert_key_valid(a, message) {
  assert_equals(indexedDB.cmp(a, a), 0, message);
}

function assert_buffer_equals(a, b, message) {
  assert_array_equals(
      Array.from(new Uint8Array(a)), Array.from(new Uint8Array(b)), message);
}

// Verifies that a JavaScript value round-trips through IndexedDB as a key.
function check_key_roundtrip_and_done(t, db, key, key_buffer) {
  const tx = db.transaction('store', 'readwrite');
  const store = tx.objectStore('store');

  // Verify put with key
  const put_request = store.put('value', key);
  put_request.onerror = t.unreached_func('put should succeed');

  // Verify get with key
  const get_request = store.get(key);
  get_request.onerror = t.unreached_func('get should succeed');
  get_request.onsuccess = t.step_func(() => {
    assert_equals(
        get_request.result, 'value',
        'get should retrieve the value given to put');

    // Verify iteration returning key
    const cursor_request = store.openCursor();
    cursor_request.onerror = t.unreached_func('openCursor should succeed');
    cursor_request.onsuccess = t.step_func(() => {
      assert_not_equals(
          cursor_request.result, null, 'cursor should be present');
      const retrieved_key = cursor_request.result.key;
      assert_true(
          retrieved_key instanceof ArrayBuffer,
          'IndexedDB binary keys should be returned in ArrayBuffer instances');
      assert_key_equals(
          retrieved_key, key,
          'The key returned by IndexedDB should equal the key given to put()');
      assert_buffer_equals(
          retrieved_key, key_buffer,
          'The ArrayBuffer returned by IndexedDB should equal the buffer ' +
              'backing the key given to put()');

      t.done();
    });
  });
}

// Checks that IndexedDB handles the given view type for binary keys correctly.
function view_type_test(type) {
  indexeddb_test(
      (t, db) => {
        db.createObjectStore('store');
      },
      (t, db) => {
        const key = new self[type](buffer);
        assert_key_valid(key, `${type} should be usable as an IndexedDB key`);
        assert_key_equals(
            key, buffer,
            'Binary keys with the same data but different view types should be ' +
                ' equal');
        check_key_roundtrip_and_done(t, db, key, buffer);
      },
      `Binary keys can be supplied using the view type ${type}`,
  );
}

['Uint8Array', 'Uint8ClampedArray', 'Int8Array', 'Uint16Array', 'Int16Array',
 'Uint32Array', 'Int32Array', 'Float16Array', 'Float32Array', 'Float64Array']
    .forEach((type) => {
      view_type_test(type);
    });

// Checks that IndexedDB
function value_test(value_description, value, value_buffer) {
  indexeddb_test(
      (t, db) => {
        db.createObjectStore('store');
      },
      (t, db) => {
        assert_key_valid(
            value, value_description + ' should be usable as an valid key');
        check_key_roundtrip_and_done(t, db, value, value_buffer);
      },
      `${value_description} can be used to supply a binary key`);
}

value_test('ArrayBuffer', buffer, buffer);
value_test('DataView', new DataView(buffer), buffer);
value_test(
    'DataView with explicit offset', new DataView(buffer, 3),
    new Uint8Array([0x11, 0xFF, 0xEE, 0xDD, 0xCC]).buffer);
value_test(
    'DataView with explicit offset and length', new DataView(buffer, 3, 4),
    new Uint8Array([0x11, 0xFF, 0xEE, 0xDD]).buffer);
value_test(
    'Uint8Array with explicit offset', new Uint8Array(buffer, 3),
    new Uint8Array([0x11, 0xFF, 0xEE, 0xDD, 0xCC]).buffer);
value_test(
    'Uint8Array with explicit offset and length', new Uint8Array(buffer, 3, 4),
    new Uint8Array([0x11, 0xFF, 0xEE, 0xDD]).buffer);
