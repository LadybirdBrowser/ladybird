// META: global=window,worker
// META: title=IDBCursor.key
// META: script=resources/support.js

'use strict';

function cursor_key(key) {
  async_test(t => {
    let db;
    const open_rq = createdb(t);
    open_rq.onupgradeneeded = function(e) {
      db = e.target.result;
      const objStore = db.createObjectStore('test');
      objStore.add('data', key);
    };

    open_rq.onsuccess = t.step_func((e) => {
      const cursor_rq =
          db.transaction('test', 'readonly').objectStore('test').openCursor();

      cursor_rq.onsuccess = t.step_func((e) => {
        const cursor = e.target.result;
        assert_equals(cursor.value, 'data', 'prerequisite cursor.value');

        assert_key_equals(cursor.key, key, 'key');
        assert_readonly(cursor, 'key');

        if (key instanceof Array) {
          cursor.key.push('new');
          key.push('new');

          assert_key_equals(cursor.key, key, 'key after array push');
        }

        t.done();
      });
    });
  });
}

cursor_key(1);
cursor_key('key');
cursor_key(['my', 'key']);
