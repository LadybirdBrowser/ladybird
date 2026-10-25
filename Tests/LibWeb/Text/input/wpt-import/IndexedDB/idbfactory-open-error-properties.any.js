// META: title=IndexedDB: Test IDBFactory open() error event properties
// META: global=window,worker
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbfactory-open

'use strict';

async_test(t => {
  const dbname = self.location + '-' + t.name;
  indexedDB.deleteDatabase(dbname);
  const open = indexedDB.open(dbname);
  open.onsuccess = t.unreached_func('open should not succeed');
  open.onupgradeneeded = t.step_func(() => {
    const tx = open.transaction;
    tx.abort();
  });
  open.onerror = t.step_func(e => {
    assert_equals(e.target, open, 'event target should be request');
    assert_equals(e.type, 'error', 'Event type should be error');
    assert_true(e.bubbles, 'Event should bubble');
    assert_true(e.cancelable, 'Event should be cancelable');
    t.done();
  });
}, 'Properties of error event from failed open()');
