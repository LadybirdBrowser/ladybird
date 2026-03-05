// META: title=IDBRequest onupgradeneeded tests
// META: global=window,worker

'use strict';

function upgradeneeded_test(
    upgrade_func, success_func, error_func, description) {
  async_test(function(t) {
    let dbName = 'db' + self.location.pathname + '-' + description;
    let delete_request = indexedDB.deleteDatabase(dbName);
    delete_request.onerror = t.unreached_func('deleteDatabase should not fail');
    delete_request.onsuccess = t.step_func(function() {
      let open_request = indexedDB.open(dbName);

      open_request.onupgradeneeded = t.step_func(function() {
        t.add_cleanup(function() {
          if (open_request.result) {
            open_request.result.close();
            indexedDB.deleteDatabase(dbName);
          }
        });
        upgrade_func(t, open_request);
      });
      open_request.onsuccess = t.step_func(function() {
        success_func(t, open_request);
      });
      if (error_func) {
        open_request.onerror = function() {
          error_func(t, open_request);
        };
      } else {
        open_request.onerror = t.unreached_func('open failed');
      }
    });
  }, description);
}

(function() {
let order = [];
upgradeneeded_test(
    function upgrade(t, request) {
      order.push('Upgrade');
      let db = request.result;
      let deleteRequest = indexedDB.deleteDatabase(db.name);
      deleteRequest.onsuccess = t.step_func(function() {
        assert_array_equals(order, ['Upgrade', 'Open Success']);
        t.done();
      });
      deleteRequest.onerror = t.unreached_func('delete failed');
    },
    function success(t, request) {
      let db = request.result;
      db.close();
      order.push('Open Success');
    },
    null, 'indexedDB.delete called from upgradeneeded handler');
}());

(function() {
let order = [];
upgradeneeded_test(
    function upgrade(t, request) {
      order.push('Upgrade');
      request.transaction.abort();
      order.push('Upgrade Transaction Aborted');
      let db = request.result;
      let deleteRequest = indexedDB.deleteDatabase(db.name);
      deleteRequest.onsuccess = t.step_func(function() {
        assert_array_equals(
            order, ['Upgrade', 'Upgrade Transaction Aborted', 'Open Error']);
        t.done();
      });
      deleteRequest.onerror = t.unreached_func('delete failed');
    },
    function success(t, request) {
      t.unreached_func('open should not succeed');
    },
    function error_func(t, request) {
      assert_array_equals(order, ['Upgrade', 'Upgrade Transaction Aborted']);
      order.push('Open Error');
    },
    'Abort transaction before deleting database in upgradeneeded handler');
}());

(function() {
let order = [];
upgradeneeded_test(
    function upgrade(t, request) {
      order.push('Upgrade');
      let db = request.result;
      let deleteRequest = indexedDB.deleteDatabase(db.name);
      request.transaction.abort();
      order.push('Upgrade Transaction Aborted');
      deleteRequest.onsuccess = t.step_func(function() {
        assert_array_equals(
            order, ['Upgrade', 'Upgrade Transaction Aborted', 'Open Error']);
        t.done();
      });
      deleteRequest.onerror = t.unreached_func('delete failed');
    },
    function success(t, request) {
      t.unreached_func('open should not succeed');
    },
    function error_func(t, request) {
      assert_array_equals(order, ['Upgrade', 'Upgrade Transaction Aborted']);
      order.push('Open Error');
    },
    'Abort transaction after deleting database in upgradeneeded event handler');
}());

(function() {
let order = [];
upgradeneeded_test(
    function upgrade(t, request) {
      order.push('Upgrade');
      let db = request.result;
      db.createObjectStore('store');
      request.transaction.oncomplete = t.step_func(function() {
        order.push('Upgrade transaction complete');
        let txn = db.transaction('store', 'readwrite');
        let store = txn.objectStore('store');
        store.put('value', 'key');
        txn.oncomplete = t.step_func(function() {
          assert_array_equals(
              order,
              ['Upgrade', 'Upgrade transaction complete', 'Open Success']);
          t.done();
        });
        txn.onerror = t.unreached_func('error on transaction');
        txn.onabort = t.unreached_func('aborting transaction');
      });
    },
    function success(t, request) {
      order.push('Open Success');
    },
    null, 'transaction oncomplete ordering relative to open request onsuccess');
}());
