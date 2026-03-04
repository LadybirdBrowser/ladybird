// META: title=IndexedDB
// META: global=window,worker
// META: script=resources/support.js

'use strict';

let saw;
indexeddb_test(
    (t, db) => {
      this.saw = expect(t, ['delete1', 'delete2']);
      let r = indexedDB.deleteDatabase(db.name);
      r.onerror = t.unreached_func('delete should succeed');
      r.onsuccess = t.step_func(e => saw('delete1'));
    },
    (t, db) => {
      let r = indexedDB.deleteDatabase(db.name);
      r.onerror = t.unreached_func('delete should succeed');
      r.onsuccess = t.step_func(e => saw('delete2'));

      db.close();
      t.done();
    },
    'Deletes are processed as a FIFO queue');
