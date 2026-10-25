// META: title=IndexedDB Transaction Event Bubbling and Capturing
// META: global=window,worker
// META: script=resources/support.js

'use strict';

async_test(t => {
  let events = [];
  let open_rq = createdb(t);
  open_rq.onupgradeneeded = function(e) {
    let db = e.target.result;
    let txn = e.target.transaction;
    let store = db.createObjectStore('store');
    let rq1 = store.add('', 1);
    let rq2 = store.add('', 1);

    // We will run db.error, but don't let that fail the test
    db.onerror = undefined;

    log_events('db', db, 'success');
    log_events('db', db, 'error');

    log_events('txn', txn, 'success');
    log_events('txn', txn, 'error');

    log_events('rq1', rq1, 'success');
    log_events('rq1', rq1, 'error');

    log_events('rq2', rq2, 'success');
    log_events('rq2', rq2, 'error');

    // Don't let it get to abort
    db.addEventListener('error', function(e) {
      e.preventDefault();
    }, false);
  };

  open_rq.onsuccess = function(e) {
    log('open_rq.success')(e);
    assert_array_equals(
        events,
        [
          'capture db.success',
          'capture txn.success',
          'capture rq1.success',
          'bubble  rq1.success',

          'capture db.error: ConstraintError',
          'capture txn.error: ConstraintError',
          'capture rq2.error: ConstraintError',
          'bubble  rq2.error: ConstraintError',
          'bubble  txn.error: ConstraintError',
          'bubble  db.error: ConstraintError',

          'open_rq.success',
        ],
        'events');
    t.done();
  };
  function log_events(type, obj, evt) {
    obj.addEventListener(evt, log('capture ' + type + '.' + evt), true);
    obj.addEventListener(evt, log('bubble  ' + type + '.' + evt), false);
  }

  function log(msg) {
    return function(e) {
      if (e && e.target && e.target.error)
        events.push(msg + ': ' + e.target.error.name);
      else
        events.push(msg);
    };
  }
}, 'Capture and bubble');
