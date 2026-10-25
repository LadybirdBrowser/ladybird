// META: global=window,worker
// META: title=IDBCursor.primaryKey
// META: script=resources/support.js

'use strict';

function cursor_primarykey(key) {
  async_test(t => {
    let db;

    const open_rq = createdb(t);
    open_rq.onupgradeneeded = t.step_func(e => {
      db = e.target.result;
      const objStore = db.createObjectStore('test');
      objStore.createIndex('index', '');

      objStore.add('data', key);
    });

    open_rq.onsuccess = t.step_func(e => {
      const cursor_rq = db.transaction('test', 'readonly')
                            .objectStore('test')
                            .index('index')
                            .openCursor();

      cursor_rq.onsuccess = t.step_func(e => {
        const cursor = e.target.result;

        assert_equals(cursor.value, 'data', 'prerequisite cursor.value');
        assert_equals(cursor.key, 'data', 'prerequisite cursor.key');

        assert_key_equals(cursor.primaryKey, key, 'primaryKey');
        assert_readonly(cursor, 'primaryKey');

        if (key instanceof Array) {
          cursor.primaryKey.push('new');
          key.push('new');

          assert_key_equals(
              cursor.primaryKey, key, 'primaryKey after array push');
        }

        t.done();
      });
    });
  });
}

cursor_primarykey(1);
cursor_primarykey('key');
cursor_primarykey(['my', 'key']);
