// META: title=IndexedDB: Test error events fired at requests from aborted transaction
// META: global=window,worker
// META: script=resources/support-promises.js
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#abort-transaction

'use strict';

indexeddb_test(
    (t, db) => {
      db.createObjectStore('store');
    },
    (t, db) => {
      const tx = db.transaction('store', 'readonly');
      const request = tx.objectStore('store').get(0);
      tx.abort();
      request.onsuccess = t.unreached_func('request should not succeed');

      let connection_saw_error = false;
      let transaction_saw_error = false;

      request.onerror = t.step_func(e => {
        assert_equals(
            request.readyState, 'done', 'Request\'s done flag should be set');
        assert_equals(
            request.result, undefined, 'Request\'s result should be undefined');
        assert_equals(
            request.error.name, 'AbortError',
            'Request\'s error should be AbortError');

        assert_equals(e.target, request, 'event target should be request');
        assert_equals(e.type, 'error', 'Event type should be error');
        assert_true(e.bubbles, 'Event should bubble');
        assert_true(e.cancelable, 'Event should cancelable');

        assert_true(
            connection_saw_error, 'Event propagated through connection');
        assert_true(
            transaction_saw_error, 'Event propagated through transaction');
        t.done();
      });

      // Event propagates via "get the parent" on request and transaction.

      db.addEventListener(
          'error', t.step_func(e => {
            connection_saw_error = true;
            assert_equals(e.target, request, 'event target should be request');
            assert_equals(e.type, 'error', 'Event type should be error');
            assert_true(e.bubbles, 'Event should bubble');
            assert_true(e.cancelable, 'Event should cancelable');
          }),
          true);

      tx.addEventListener(
          'error', t.step_func(e => {
            transaction_saw_error = true;
            assert_equals(e.target, request, 'event target should be request');
            assert_equals(e.type, 'error', 'Event type should be error');
            assert_true(e.bubbles, 'Event should bubble');
            assert_true(e.cancelable, 'Event should cancelable');

            assert_true(
                connection_saw_error, 'Event propagated through connection');
          }),
          true);
    },
    'Properties of error events fired at requests when aborting a transaction');
