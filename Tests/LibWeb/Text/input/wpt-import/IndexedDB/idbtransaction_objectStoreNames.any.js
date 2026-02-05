// META: title=IndexedDB: IDBTransaction.objectStoreNames attribute
// META: global=window,worker
// META: script=resources/support.js

'use strict';

function with_stores_test(store_names, open_func, description) {
  indexeddb_test(function(t, db, tx) {
    store_names.forEach(function(name) {
      db.createObjectStore(name);
    });
  }, open_func, description);
}

indexeddb_test(
    function(t, db, tx) {
      assert_array_equals(
          tx.objectStoreNames, [],
          'transaction objectStoreNames should be empty');
      assert_array_equals(
          db.objectStoreNames, tx.objectStoreNames,
          'connection and transacton objectStoreNames should match');

      db.createObjectStore('s1');
      assert_array_equals(
          tx.objectStoreNames, ['s1'],
          'transaction objectStoreNames should have new store');
      assert_array_equals(
          db.objectStoreNames, tx.objectStoreNames,
          'connection and transacton objectStoreNames should match');

      db.createObjectStore('s3');
      assert_array_equals(
          tx.objectStoreNames, ['s1', 's3'],
          'transaction objectStoreNames should have new store');
      assert_array_equals(
          db.objectStoreNames, tx.objectStoreNames,
          'connection and transacton objectStoreNames should match');

      db.createObjectStore('s2');
      assert_array_equals(
          tx.objectStoreNames, ['s1', 's2', 's3'],
          'transaction objectStoreNames should be sorted');
      assert_array_equals(
          db.objectStoreNames, tx.objectStoreNames,
          'connection and transacton objectStoreNames should match');

      db.deleteObjectStore('s1');
      assert_array_equals(
          tx.objectStoreNames, ['s2', 's3'],
          'transaction objectStoreNames should be updated after delete');
      assert_array_equals(
          db.objectStoreNames, tx.objectStoreNames,
          'connection and transacton objectStoreNames should match');
    },
    function(t, db) {
      t.done();
    },
    'IDBTransaction.objectStoreNames - during upgrade transaction');

(function() {
let saved_tx;
indexeddb_test(
    function(t, db, tx) {
      saved_tx = tx;
      db.createObjectStore('s2');
      db.createObjectStore('s3');
    },
    function(t, db) {
      db.close();
      let open2 = indexedDB.open(db.name, db.version + 1);
      open2.onerror = t.unreached_func('open should succeed');
      open2.onupgradeneeded = t.step_func(function() {
        let db2 = open2.result;
        let tx2 = open2.transaction;
        assert_array_equals(
            tx2.objectStoreNames, ['s2', 's3'],
            'transaction should have previous stores in scope');
        assert_array_equals(
            db2.objectStoreNames, tx2.objectStoreNames,
            'connection and transacton objectStoreNames should match');

        db2.createObjectStore('s4');
        assert_array_equals(
            tx2.objectStoreNames, ['s2', 's3', 's4'],
            'transaction should have new store in scope');
        assert_array_equals(
            db2.objectStoreNames, tx2.objectStoreNames,
            'connection and transacton objectStoreNames should match');

        assert_array_equals(
            saved_tx.objectStoreNames, ['s2', 's3'],
            'previous transaction objectStoreNames should be unchanged');
        assert_array_equals(
            db.objectStoreNames, saved_tx.objectStoreNames,
            'connection and transaction objectStoreNames should match');
        db2.close();
        t.done();
      });
    },
    'IDBTransaction.objectStoreNames - value after close');
}());

with_stores_test(['s1', 's2'], function(t, db) {
  assert_array_equals(
      db.transaction('s1', 'readonly').objectStoreNames, ['s1'],
      'transaction should have one store in scope');
  assert_array_equals(
      db.transaction(['s1', 's2']).objectStoreNames, ['s1', 's2'],
      'transaction should have two stores in scope');
  t.done();
}, 'IDBTransaction.objectStoreNames - transaction scope');

with_stores_test(['s1', 's2'], function(t, db) {
  let tx = db.transaction(['s1', 's2'], 'readwrite');
  tx.objectStore('s1').put(0, 0);
  tx.onabort = t.unreached_func('transaction should complete');
  tx.oncomplete = t.step_func(function() {
    assert_array_equals(
        tx.objectStoreNames, ['s1', 's2'],
        'objectStoreNames should return scope after transaction commits');
    t.done();
  });
}, 'IDBTransaction.objectStoreNames - value after commit');

with_stores_test(['s1', 's2'], function(t, db) {
  let tx = db.transaction(['s1', 's2'], 'readwrite');
  tx.objectStore('s1').put(0, 0);
  tx.objectStore('s1').add(0, 0);
  tx.oncomplete = t.unreached_func('transaction should abort');
  tx.onabort = t.step_func(function() {
    assert_array_equals(
        tx.objectStoreNames, ['s1', 's2'],
        'objectStoreNames should return scope after transaction aborts');
    t.done();
  });
}, 'IDBTransaction.objectStoreNames - value after abort');

with_stores_test(['s1', 's2', 's3'], function(t, db) {
  assert_array_equals(
      db.transaction(['s3', 's2', 's1']).objectStoreNames, ['s1', 's2', 's3'],
      'transaction objectStoreNames should be sorted');
  t.done();
}, 'IDBTransaction.objectStoreNames - sorting');

with_stores_test(['s1', 's2'], function(t, db) {
  assert_array_equals(
      db.transaction(['s2', 's1', 's2']).objectStoreNames, ['s1', 's2'],
      'transaction objectStoreNames should not have duplicates');
  t.done();
}, 'IDBTransaction.objectStoreNames - no duplicates');

let unusual_names = [
  '',  // empty string

  '\x00',  // U+0000 NULL
  '\xFF',  // U+00FF LATIN SMALL LETTER Y WITH DIAERESIS

  '1',    // basic ASCII
  '12',   // basic ASCII
  '123',  // basic ASCII
  'abc',  // basic ASCII
  'ABC',  // basic ASCII

  '\xA2',          // U+00A2 CENT SIGN
  '\u6C34',        // U+6C34 CJK UNIFIED IDEOGRAPH (water)
  '\uD834\uDD1E',  // U+1D11E MUSICAL SYMBOL G-CLEF (UTF-16 surrogate pair)
  '\uFFFD',        // U+FFFD REPLACEMENT CHARACTER

  '\uD800',  // UTF-16 surrogate lead
  '\uDC00',  // UTF-16 surrogate trail
];
unusual_names.sort();

indexeddb_test(
    function(t, db, tx) {
      unusual_names.slice().reverse().forEach(function(name) {
        db.createObjectStore(name);
      });
      assert_array_equals(
          tx.objectStoreNames, unusual_names,
          'transaction should have names sorted');
    },
    function(t, db) {
      let tx =
          db.transaction(unusual_names.slice().reverse().concat(unusual_names));
      assert_array_equals(
          tx.objectStoreNames, unusual_names,
          'transaction should have names sorted with no duplicates');
      t.done();
    },
    'IDBTransaction.objectStoreNames - unusual names');
