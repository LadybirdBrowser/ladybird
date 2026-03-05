// META: title=Event order when opening a second database when one connection is open already
// META: global=window,worker
// META: script=resources/support.js

'use strict';

async function setupDatabase(t, dbname, version) {
  indexedDB.deleteDatabase(dbname);

  const openrq = indexedDB.open(dbname, version);
  const eventWatcher = new EventWatcher(
      t, openrq, ['upgradeneeded', 'error', 'blocked', 'success']);

  let event = await eventWatcher.wait_for('upgradeneeded');
  const db = event.target.result;
  db.createObjectStore('store');

  await eventWatcher.wait_for('success');
  return db;
}

promise_test(async t => {
  const dbname = location + '-' + t.name;
  const version = 3;
  const db = await setupDatabase(t, dbname, version);
  let db2;

  t.add_cleanup(() => {
    if (db2)
      db2.close();
    if (db)
      db.close();
    indexedDB.deleteDatabase(dbname);
  });

  const dbWatcher = new EventWatcher(t, db, ['versionchange', 'close']);
  const openrq2 = indexedDB.open(dbname, version + 1);
  let versionChangeEvent = await dbWatcher.wait_for('versionchange');
  const openrq2Watcher = new EventWatcher(
      t, openrq2, ['upgradeneeded', 'success', 'error', 'blocked']);

  assert_equals(versionChangeEvent.oldVersion, version, 'old version');
  assert_equals(versionChangeEvent.newVersion, version + 1, 'new version');
  db.close();

  await openrq2Watcher.wait_for('upgradeneeded');

  let successEvent = await openrq2Watcher.wait_for('success');
  db2 = successEvent.target.result;
}, 'No Blocked event');

promise_test(async t => {
  const dbname = location + '-' + t.name;
  const version = 3;
  const db = await setupDatabase(t, dbname, version);
  let db2;

  t.add_cleanup(() => {
    if (db2)
      db2.close();
    if (db)
      db.close();
    indexedDB.deleteDatabase(dbname);
  });

  const dbWatcher = new EventWatcher(t, db, ['versionchange', 'close']);
  const openrq2 = indexedDB.open(dbname, version + 1);
  let versionChangeEvent = await dbWatcher.wait_for('versionchange');
  const openrq2Watcher = new EventWatcher(
      t, openrq2, ['blocked', 'upgradeneeded', 'error', 'success']);

  assert_equals(versionChangeEvent.oldVersion, version, 'old version');
  assert_equals(versionChangeEvent.newVersion, version + 1, 'new version');

  let blockedEvent = await openrq2Watcher.wait_for('blocked');
  db.close();

  await openrq2Watcher.wait_for('upgradeneeded');

  let successEvent = await openrq2Watcher.wait_for('success');
  db2 = successEvent.target.result;
}, 'Blocked event');
