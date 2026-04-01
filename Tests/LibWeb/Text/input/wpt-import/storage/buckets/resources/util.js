'use strict';

// Makes sure initial bucket state is as expected and to clean up after the test
// is over (whether it passes or fails).
async function prepareForBucketTest(test) {
  // Verify initial state.
  assert_equals((await navigator.storageBuckets.keys()).join(), '');
  // Clean up after test.
  test.add_cleanup(async function() {
    const keys = await navigator.storageBuckets.keys();
    for (const key of keys) {
      await navigator.storageBuckets.delete(key);
    }
  });
}

function indexedDbOpenRequest(t, idb, dbname, upgrade_func) {
  return new Promise((resolve, reject) => {
    const openRequest = idb.open(dbname);
    t.add_cleanup(() => {
      indexedDbDeleteRequest(idb, dbname);
    });

    openRequest.onerror = () => {
      reject(openRequest.error);
    };
    openRequest.onsuccess = () => {
      resolve(openRequest.result);
    };
    openRequest.onupgradeneeded = event => {
      upgrade_func(openRequest.result);
    };
  });
}

function indexedDbDeleteRequest(idb, name) {
  return new Promise((resolve, reject) => {
    const deleteRequest = idb.deleteDatabase(name);
    deleteRequest.onerror = () => {
      reject(deleteRequest.error);
    };
    deleteRequest.onsuccess = () => {
      resolve();
    };
  });
}

function transactionPromise(txn) {
  return new Promise((resolve, reject) => {
    txn.onabort = () => {
      reject(txn.error);
    };
    txn.oncomplete = () => {
      resolve();
    };
  });
}
