// META: global=window,worker
// META: title=IDBCursor.continue()
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbcursor-continue

'use strict';

const store = [
    { value: 'cupcake', key: 5 },
    { value: 'pancake', key: 3 },
    { value: 'pie', key: 1 },
    { value: 'pie', key: 4 },
    { value: 'taco', key: 2 }
];

function upgrade_func(t, db, tx) {
  let os;
  let i;
  os = db.createObjectStore('test');
  os.createIndex('index', '');

  for (i = 0; i < store.length; i++)
    os.add(store[i].value, store[i].key);
}

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor();

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 5, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    assert_equals(cursor.value, store[count].value);
    assert_equals(cursor.primaryKey, store[count].key);

    cursor.continue();

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.continue() - continues');


indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor();

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 3, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'cupcake');
        assert_equals(cursor.primaryKey, 5);
        cursor.continue('pie');
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        cursor.continue('taco');
        break;

      case 2:
        assert_equals(cursor.value, 'taco');
        assert_equals(cursor.primaryKey, 2);
        cursor.continue();
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.continue() - with given key');


indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor();

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 1, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'cupcake');
        assert_equals(cursor.primaryKey, 5);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.continue([]);  // Arrays are always bigger than strings
  });
  rq.onerror = t.unreached_func('unexpected error2');
}, 'IDBCursor.continue() - skip far forward');


indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor(IDBKeyRange.lowerBound('cupcake', true));

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 2, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pancake');
        assert_equals(cursor.primaryKey, 3);
        cursor.continue('pie');
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        cursor.continue('zzz');
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error1');
}, 'IDBCursor.continue() - within range');


indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor('pancake');

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 1, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pancake');
        assert_equals(cursor.primaryKey, 3);
        cursor.continue('pie');
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error1');
}, 'IDBCursor.continue() - within single key range');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  const rq = db.transaction('test', 'readonly')
                 .objectStore('test')
                 .index('index')
                 .openCursor('pie');

  rq.onsuccess = t.step_func((e) => {
    if (!e.target.result) {
      assert_equals(count, 2, 'count');
      t.done();
      return;
    }
    const cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        cursor.continue();
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 4);
        cursor.continue();
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error1');
}, 'IDBCursor.continue() - within single key range, with several results');
