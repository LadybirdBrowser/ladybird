// META: global=window,worker
// META: title=Fire error event - Exception thrown
// META: script=resources/support.js

// Spec: "https://w3c.github.io/IndexedDB/#fire-error-event"

'use strict';

setup({allow_uncaught_exception: true});

function fire_error_event_test(func, description) {
  indexeddb_test(
      (t, db) => {
        db.createObjectStore('s');
      },
      (t, db) => {
        const tx = db.transaction('s', 'readwrite');
        tx.oncomplete = t.unreached_func('transaction should abort');
        const store = tx.objectStore('s');
        store.put(0, 0);
        const request = store.add(0, 0);
        request.onsuccess = t.unreached_func('request should fail');
        func(t, db, tx, request);
        tx.addEventListener('abort', t.step_func_done(() => {
          assert_equals(tx.error.name, 'AbortError');
        }));
      },
      description);
}

// Listeners on the request.

fire_error_event_test((t, db, tx, request) => {
  request.onerror = () => {
    throw Error();
  };
}, 'Exception in error event handler on request');

fire_error_event_test((t, db, tx, request) => {
  request.onerror = e => {
    e.preventDefault();
    throw Error();
  };
}, 'Exception in error event handler on request, with preventDefault');

fire_error_event_test((t, db, tx, request) => {
  request.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in error event listener on request');

fire_error_event_test((t, db, tx, request) => {
  request.addEventListener('error', {
    get handleEvent() {
      throw new Error();
    },
  });
}, 'Exception in error event listener ("handleEvent" lookup) on request');

fire_error_event_test((t, db, tx, request) => {
  request.addEventListener('error', {});
}, 'Exception in error event listener (non-callable "handleEvent") on request');

fire_error_event_test((t, db, tx, request) => {
  request.addEventListener(
      'error',
      () => {
          // no-op
      });
  request.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in second error event listener on request');

fire_error_event_test(
    (t, db, tx, request) => {
      let second_listener_called = false;
      request.addEventListener('error', () => {
        throw Error();
      });
      request.addEventListener('error', t.step_func(() => {
        second_listener_called = true;
        assert_true(
            is_transaction_active(tx, 's'),
            'Transaction should be active until dispatch completes');
      }));
      tx.addEventListener('abort', t.step_func(() => {
        assert_true(second_listener_called);
      }));
    },
    'Exception in first error event listener on request, ' +
        'transaction active in second');

// Listeners on the transaction.

fire_error_event_test((t, db, tx, request) => {
  tx.onerror = () => {
    throw Error();
  };
}, 'Exception in error event handler on transaction');

fire_error_event_test((t, db, tx, request) => {
  tx.onerror = e => {
    e.preventDefault();
    throw Error();
  };
}, 'Exception in error event handler on transaction, with preventDefault');

fire_error_event_test((t, db, tx, request) => {
  tx.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in error event listener on transaction');

fire_error_event_test((t, db, tx, request) => {
  tx.addEventListener(
      'error',
      () => {
          // no-op
      });
  tx.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in second error event listener on transaction');

fire_error_event_test(
    (t, db, tx, request) => {
      let second_listener_called = false;
      tx.addEventListener('error', () => {
        throw Error();
      });
      tx.addEventListener('error', t.step_func(() => {
        second_listener_called = true;
        assert_true(
            is_transaction_active(tx, 's'),
            'Transaction should be active until dispatch completes');
      }));
      tx.addEventListener('abort', t.step_func(() => {
        assert_true(second_listener_called);
      }));
    },
    'Exception in first error event listener on transaction, ' +
        'transaction active in second');

// Listeners on the connection.

fire_error_event_test((t, db, tx, request) => {
  db.onerror = () => {
    throw Error();
  };
}, 'Exception in error event handler on connection');

fire_error_event_test((t, db, tx, request) => {
  db.onerror = e => {
    e.preventDefault()
    throw Error();
  };
}, 'Exception in error event handler on connection, with preventDefault');

fire_error_event_test((t, db, tx, request) => {
  db.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in error event listener on connection');

fire_error_event_test((t, db, tx, request) => {
  db.addEventListener(
      'error',
      () => {
          // no-op
      });
  db.addEventListener('error', () => {
    throw Error();
  });
}, 'Exception in second error event listener on connection');

fire_error_event_test(
    (t, db, tx, request) => {
      let second_listener_called = false;
      db.addEventListener('error', () => {
        throw Error();
      });
      db.addEventListener('error', t.step_func(() => {
        second_listener_called = true;
        assert_true(
            is_transaction_active(tx, 's'),
            'Transaction should be active until dispatch completes');
      }));
      tx.addEventListener('abort', t.step_func(() => {
        assert_true(second_listener_called);
      }));
    },
    'Exception in first error event listener on connection, ' +
        'transaction active in second');
