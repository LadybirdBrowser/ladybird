// META: global=window,worker
// META: title=Fire upgradeneeded event - Exception thrown
// META: script=resources/support.js

// Spec: "https://w3c.github.io/IndexedDB/#fire-a-version-change-event"

setup({allow_uncaught_exception: true});

function fire_upgradeneeded_event_test(func, description) {
  async_test(t => {
    const dbname = self.location + '-' + t.name;
    const del = indexedDB.deleteDatabase(dbname);
    del.onerror = t.unreached_func('deleteDatabase should succeed');
    const open = indexedDB.open(dbname, 1);
    open.onsuccess = t.unreached_func('open should fail');
    let tx;
    open.addEventListener('upgradeneeded', () => {
      tx = open.transaction;
    });
    func(t, open);
    open.addEventListener('error', t.step_func_done(() => {
      assert_equals(tx.error.name, 'AbortError');
    }));
  }, description);
}

fire_upgradeneeded_event_test((t, open) => {
  open.onupgradeneeded = () => {
    throw Error();
  };
}, 'Exception in upgradeneeded handler');

fire_upgradeneeded_event_test((t, open) => {
  open.addEventListener('upgradeneeded', () => {
    throw Error();
  });
}, 'Exception in upgradeneeded listener');

fire_upgradeneeded_event_test((t, open) => {
  open.addEventListener('upgradeneeded', {
    get handleEvent() {
      throw new Error();
    },
  });
}, 'Exception in upgradeneeded "handleEvent" lookup');

fire_upgradeneeded_event_test((t, open) => {
  open.addEventListener('upgradeneeded', {
    get handleEvent() {
      return 10;
    },
  });
}, 'Exception in upgradeneeded due to non-callable "handleEvent"');

fire_upgradeneeded_event_test((t, open) => {
  open.addEventListener(
      'upgradeneeded',
      () => {
          // No-op.
      });
  open.addEventListener('upgradeneeded', () => {
    throw Error();
  });
}, 'Exception in second upgradeneeded listener');

fire_upgradeneeded_event_test((t, open) => {
  let second_listener_called = false;
  open.addEventListener('upgradeneeded', () => {
    open.result.createObjectStore('s');
    throw Error();
  });
  open.addEventListener('upgradeneeded', t.step_func(() => {
    second_listener_called = true;
    assert_true(
        is_transaction_active(open.transaction, 's'),
        'Transaction should be active until dispatch completes');
  }));
  open.addEventListener('error', t.step_func(() => {
    assert_true(second_listener_called);
  }));
}, 'Exception in first upgradeneeded listener, tx active in second');
