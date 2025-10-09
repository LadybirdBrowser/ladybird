// META: global=window,worker
// META: title=IDBCursor.advance()
// META: script=resources/support.js

'use strict';

function upgrade_func(t, db, tx) {
  let objStore = db.createObjectStore('test');
  objStore.createIndex('index', '');

  objStore.add('cupcake', 5);
  objStore.add('pancake', 3);
  objStore.add('pie', 1);
  objStore.add('pie', 4);
  objStore.add('taco', 2);
}

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 3, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'cupcake');
        assert_equals(cursor.primaryKey, 5);
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        break;

      case 2:
        assert_equals(cursor.value, 'taco');
        assert_equals(cursor.primaryKey, 2);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(2);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - advances');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor(null, 'prev');

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 3, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'taco');
        assert_equals(cursor.primaryKey, 2);
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        break;

      case 2:
        assert_equals(cursor.value, 'cupcake');
        assert_equals(cursor.primaryKey, 5);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(2);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - advances backwards');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 1, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'cupcake');
        assert_equals(cursor.primaryKey, 5);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(100000);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - skip far forward');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor(IDBKeyRange.lowerBound('cupcake', true));

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 2, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pancake');
        assert_equals(cursor.primaryKey, 3);
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 4);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(2);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - within range');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor('pancake');

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 1, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pancake');
        assert_equals(cursor.primaryKey, 3);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(1);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - within single key range');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor('pie');

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 2, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    switch (count) {
      case 0:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 1);
        break;

      case 1:
        assert_equals(cursor.value, 'pie');
        assert_equals(cursor.primaryKey, 4);
        break;

      default:
        assert_unreached('Unexpected count: ' + count);
    }

    count++;
    cursor.advance(1);
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - within single key range, with several results');
