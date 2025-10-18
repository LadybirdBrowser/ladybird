// META: global=window,worker
// META: title=Fire success event - Exception thrown
// META: script=resources/support.js

// Spec: "https://w3c.github.io/IndexedDB/#fire-success-event"

'use strict';

setup({allow_uncaught_exception: true});

function fire_success_event_test(func, description) {
  indexeddb_test(
      (t, db) => {
        db.createObjectStore('s');
      },
      (t, db) => {
        const tx = db.transaction('s', 'readonly');
        tx.oncomplete = t.unreached_func('transaction should abort');
        const store = tx.objectStore('s');
        const request = store.get(0);
        func(t, db, tx, request);
        tx.addEventListener('abort', t.step_func_done(() => {
          assert_equals(tx.error.name, 'AbortError');
        }));
      },
      description);
}

fire_success_event_test((t, db, tx, request) => {
  request.onsuccess = () => {
    throw Error();
  };
}, 'Exception in success event handler on request');

fire_success_event_test((t, db, tx, request) => {
  request.addEventListener('success', () => {
    throw Error();
  });
}, 'Exception in success event listener on request');

fire_success_event_test((t, db, tx, request) => {
  request.addEventListener('success', {
    get handleEvent() {
      throw new Error();
    },
  });
}, 'Exception in success event listener ("handleEvent" lookup) on request');

fire_success_event_test((t, db, tx, request) => {
  request.addEventListener('success', {
    handleEvent: null,
  });
}, 'Exception in success event listener (non-callable "handleEvent") on request');

fire_success_event_test((t, db, tx, request) => {
  request.addEventListener(
      'success',
      () => {
          // no-op
      });
  request.addEventListener('success', () => {
    throw Error();
  });
}, 'Exception in second success event listener on request');

fire_success_event_test((t, db, tx, request) => {
  let second_listener_called = false;
  request.addEventListener('success', () => {
    throw Error();
  });
  request.addEventListener('success', t.step_func(() => {
    second_listener_called = true;
    assert_true(
        is_transaction_active(tx, 's'),
        'Transaction should be active until dispatch completes');
  }));
  tx.addEventListener('abort', t.step_func(() => {
    assert_true(second_listener_called);
  }));
}, 'Exception in first success event listener, tx active in second');
