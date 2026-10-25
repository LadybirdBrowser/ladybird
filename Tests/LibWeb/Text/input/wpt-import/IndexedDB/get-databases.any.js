// META: script=resources/support-promises.js
'use strict';

promise_test(async testCase => {
  let result = indexedDB.databases();
  assert_true(result instanceof Promise,
      "databases() should return a promise.");
  result.catch(() => {});
}, "Ensure that databases() returns a promise.");

promise_test(async testCase => {
  // Delete any databases that may not have been cleaned up after previous test
  // runs.
  await deleteAllDatabases(testCase);

  const db_name = "TestDatabase";
  const db = await createNamedDatabase(testCase, db_name, ()=>{});
  const databases_result = await indexedDB.databases();
  db.close();
  const expected_result = {"name": db_name, "version": 1};
  assert_equals(
      databases_result.length,
      1,
      "The result of databases() should contain one result per database.");
  assert_true(
      databases_result[0].name === expected_result.name
          && databases_result[0].version === expected_result.version,
      "The result of databases() should be a sequence of the correct names "
      + "and versions of all databases for the origin.");
}, "Enumerate one database.");

promise_test(async testCase => {
  // Delete any databases that may not have been cleaned up after previous test
  // runs.
  await deleteAllDatabases(testCase);

  const db_name1 = "TestDatabase1";
  const db_name2 = "TestDatabase2";
  const db_name3 = "TestDatabase3";
  const db1 = await createNamedDatabase(testCase, db_name1, ()=>{});
  const db2 = await createNamedDatabase(testCase, db_name2, ()=>{});
  const db3 = await createNamedDatabase(testCase, db_name3, ()=>{});
  db1.close();
  db2.close();
  db3.close();
  const version_promise =
      await migrateNamedDatabase(testCase, db_name2, 2, () => {});
  const databases_result = await indexedDB.databases();
  const expected_result = [
    {"name": db_name1, "version": 1},
    {"name": db_name2, "version": 2},
    {"name": db_name3, "version": 1},
  ];
  assert_equals(
      databases_result.length,
      expected_result.length,
      "The result of databases() should contain one result per database.");
  for ( let i = 0; i < expected_result.length; i += 1 ) {
    const result = expected_result[i];
    assert_true(
        databases_result.some(
            e => e.name === result.name && e.version === result.version),
        "The result of databases() should be a sequence of the correct names "
        + "and versions of all databases for the origin.");
  }
}, "Enumerate multiple databases.");

promise_test(async testCase => {
  // Add some databases and close their connections.
  const db1 = await createNamedDatabase(testCase, "DB1", () => {});
  const db2 = await createNamedDatabase(testCase, "DB2", () => {});
  db1.close();
  db2.close();

  // Delete any databases that may not have been cleaned up after previous test
  // runs as well as the two databases made above.
  await deleteAllDatabases(testCase);

  // Make sure the databases are no longer returned.
  const databases_result = await indexedDB.databases();
  assert_equals(
      databases_result.length,
      0,
      "The result of databases() should be an empty sequence for the case of "
      + "no databases for the origin.");
}, "Make sure an empty list is returned for the case of no databases.");

promise_test(async testCase => {
  function sleep_sync(msec) {
    const start = new Date().getTime();
    while (new Date().getTime() - start < msec) {}
  }

  // Delete any databases that may not have been cleaned up after previous test
  // runs as well as the two databases made above.
  await deleteAllDatabases(testCase);

  const db1 = await createNamedDatabase(testCase, "DB1", ()=>{});
  let databases_promise1;
  const db2 = await createNamedDatabase(testCase, "DB2", async () => {
    databases_promise1 = indexedDB.databases();

    // Give databases() operation a chance to fetch all current info about
    // existing databases. This must be a sync sleep since await would trigger
    // auto commit of the upgrade transaction.
    sleep_sync(1000);
  });
  const databases_result1 = await databases_promise1;
  assert_equals(
      databases_result1.length,
      1,
      "The result of databases() should be only those databases which have "
      + "been created at the time of calling, regardless of versionchange "
      + "transactions currently running.");
  db1.close();
  db2.close();
  const databases_result2 = await indexedDB.databases();
  assert_equals(
      databases_result2.length,
      2,
      "The result of databases() should include all databases which have "
      + "been created at the time of calling.");
  let databases_promise3;
  await migrateNamedDatabase(testCase, "DB2", 2, async () => {
    databases_promise3 = indexedDB.databases();

    // Give databases() operation a chance to fetch all current info about
    // existing databases. This must be a sync sleep since await would trigger
    // auto commit of the upgrade transaction.
    sleep_sync(1000);
  });
  const databases_result3 = await databases_promise3;
  assert_true(
      databases_result3[0].version === 1
      && databases_result3[1].version === 1,
      "The result of databases() should contain the versions of databases "
      + "at the time of calling, regardless of versionchange transactions "
      + "currently running.");
}, "Ensure that databases() doesn't pick up changes that haven't commited.");
