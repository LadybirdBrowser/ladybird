// META: title=IndexedDB: Commit ordering of empty transactions
// META: global=window,worker
// META: script=resources/support.js

'use strict';

indexeddb_test(
    (t, db) => {
      db.createObjectStore('store');
    },
    (t, db) => {
      let saw = expect(t, [
        'rq1.onsuccess', 'rq2.onsuccess', 'tx1.oncomplete', 'tx2.oncomplete'
      ]);

      let tx1 = db.transaction('store', 'readwrite');
      tx1.onabort = t.unreached_func('transaction should commit');
      tx1.oncomplete = t.step_func(() => saw('tx1.oncomplete'));

      let store = tx1.objectStore('store');
      let rq1 = store.put('a', 1);
      rq1.onerror = t.unreached_func('put should succeed');
      rq1.onsuccess = t.step_func(() => {
        saw('rq1.onsuccess');

        let tx2 = db.transaction('store', 'readonly');
        tx2.onabort = t.unreached_func('transaction should commit');
        tx2.oncomplete = t.step_func(() => saw('tx2.oncomplete'));

        let rq2 = store.put('b', 2);
        rq2.onsuccess = t.step_func(() => saw('rq2.onsuccess'));
        rq2.onerror = t.unreached_func('request should succeed');
      });
    },
    'Transactions without requests complete in the expected order');

indexeddb_test(
    (t, db) => {
      db.createObjectStore('store');
    },
    (t, db) => {
      let saw = expect(t, [
        'rq1.onsuccess', 'rq2.onsuccess', 'tx1.oncomplete', 'tx2.oncomplete',
        'tx3.oncomplete'
      ]);
      let tx1 = db.transaction('store', 'readwrite');
      tx1.onabort = t.unreached_func('transaction should commit');
      tx1.oncomplete = t.step_func(() => saw('tx1.oncomplete'));

      let store = tx1.objectStore('store');
      let rq1 = store.put('a', 1);
      rq1.onerror = t.unreached_func('put should succeed');
      rq1.onsuccess = t.step_func(() => {
        saw('rq1.onsuccess');

        let tx2 = db.transaction('store', 'readonly');
        tx2.onabort = t.unreached_func('transaction should commit');
        tx2.oncomplete = t.step_func(() => saw('tx2.oncomplete'));

        let tx3 = db.transaction('store', 'readonly');
        tx3.onabort = t.unreached_func('transaction should commit');
        tx3.oncomplete = t.step_func(() => saw('tx3.oncomplete'));

        let rq2 = store.put('b', 2);
        rq2.onsuccess = t.step_func(() => saw('rq2.onsuccess'));
        rq2.onerror = t.unreached_func('request should succeed');
      });
    },
    'Multiple transactions without requests complete in the expected order');
