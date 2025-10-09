// META: global=window,worker
// META: title=IDBCursor.advance() - invalid
// META: script=resources/support.js

// Spec:
// https://w3c.github.io/IndexedDB/#widl-IDBCursor-advance-void-unsigned-long-count

'use strict';

function upgrade_func(t, db, tx) {
  let objStore = db.createObjectStore('test');
  objStore.createIndex('index', '');

  objStore.add('data', 1);
  objStore.add('data2', 2);
}

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    if (!e.target.result) {
      assert_equals(count, 2, 'count');
      t.done();
      return;
    }
    let cursor = e.target.result;

    cursor.advance(1);

    // Second try
    assert_throws_dom('InvalidStateError', function() {
      cursor.advance(1);
    }, 'second advance');

    assert_throws_dom('InvalidStateError', function() {
      cursor.advance(3);
    }, 'third advance');

    count++;
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - attempt to call advance twice');

indexeddb_test(upgrade_func, function(t, db) {
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    let cursor = e.target.result;

    assert_throws_js(TypeError, function() {
      cursor.advance(self);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance({});
    });

    assert_throws_js(TypeError, function() {
      cursor.advance([]);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance('');
    });

    assert_throws_js(TypeError, function() {
      cursor.advance('1 2');
    });

    t.done();
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - pass something other than number');


indexeddb_test(upgrade_func, function(t, db) {
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    let cursor = e.target.result;

    assert_throws_js(TypeError, function() {
      cursor.advance(null);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(undefined);
    });

    let mylet = null;
    assert_throws_js(TypeError, function() {
      cursor.advance(mylet);
    });

    t.done();
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - pass null/undefined');


indexeddb_test(upgrade_func, function(t, db) {
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    let cursor = e.target.result;

    assert_throws_js(TypeError, function() {
      cursor.advance();
    });

    t.done();
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - missing argument');

indexeddb_test(upgrade_func, function(t, db) {
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    let cursor = e.target.result;

    assert_throws_js(TypeError, function() {
      cursor.advance(-1);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(NaN);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(0);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(-0);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(Infinity);
    });

    assert_throws_js(TypeError, function() {
      cursor.advance(-Infinity);
    });

    let mylet = -999999;
    assert_throws_js(TypeError, function() {
      cursor.advance(mylet);
    });

    t.done();
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - pass negative numbers');

indexeddb_test(upgrade_func, function(t, db) {
  let count = 0;
  let rq = db.transaction('test', 'readonly')
               .objectStore('test')
               .index('index')
               .openCursor();

  rq.onsuccess = t.step_func(function(e) {
    let cursor = e.target.result;
    if (!cursor) {
      assert_equals(count, 2, 'count runs');
      t.done();
      return;
    }

    assert_throws_js(TypeError, function() {
      cursor.advance(0);
    });

    cursor.advance(1);
    count++;
  });
  rq.onerror = t.unreached_func('unexpected error');
}, 'IDBCursor.advance() - invalid - got value not set on exception');
