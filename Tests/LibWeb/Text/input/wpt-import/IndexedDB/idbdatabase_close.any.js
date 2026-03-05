// META: global=window,worker
// META: title=IDBDatabase.close()
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbdatabase-transaction

'use strict';

async_test(t => {
  let db;
  let versionchange_fired;
  let blocked_fired;
  let upgradeneeded_fired;
  const open_rq = createdb(t);
  let counter = 0;

  open_rq.onupgradeneeded = function() {};
  open_rq.onsuccess = function(e) {
    db = e.target.result;
    db.onversionchange = t.step_func((e) => {
      versionchange_fired = counter++;
    });
    const rq = indexedDB.open(db.name, db.version + 1);
    rq.onblocked = t.step_func((e) => {
      blocked_fired = counter++;
      db.close();
    });
    rq.onupgradeneeded = t.step_func((e) => {
      upgradeneeded_fired = counter++;
    });
    rq.onsuccess = t.step_func((e) => {
      assert_equals(versionchange_fired, 0, 'versionchange event fired #');
      assert_equals(blocked_fired, 1, 'block event fired #');
      assert_equals(
          upgradeneeded_fired, 2, 'second upgradeneeded event fired #');

      rq.result.close();
      t.done();
    });
    rq.onerror = t.step_func(e => {
      assert_unreached('Unexpected database deletion error: ' + e.target.error);
    });
  };
}, 'Unblock the version change transaction created by an open database request');

async_test(t => {
  let db;
  let blocked_fired = false;
  let versionchange_fired = false;
  const open_rq = createdb(t);

  open_rq.onupgradeneeded = t.step_func(e => {});
  open_rq.onsuccess = t.step_func(e => {
    db = e.target.result;

    db.onversionchange = t.step_func(e => {
      versionchange_fired = true;
    });

    const rq = indexedDB.deleteDatabase(db.name);
    rq.onblocked = t.step_func(e => {
      blocked_fired = true;
      db.close();
    });
    rq.onsuccess = t.step_func(e => {
      assert_true(versionchange_fired, 'versionchange event fired')
      assert_true(blocked_fired, 'block event fired')
      t.done();
    });
    rq.onerror = t.step_func(e => {
      assert_unreached('Unexpected database deletion error: ' + e.target.error);
    });
  });
}, 'Unblock the delete database request.');
