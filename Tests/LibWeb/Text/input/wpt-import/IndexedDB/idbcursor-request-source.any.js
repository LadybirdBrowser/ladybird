// META: global=window,worker
// META: title=IndexedDB: The source of requests made against cursors
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#dom-idbrequest-source

'use strict';

// Setup each test by populating an object store with an index for the cursor to
// iterate and manipulate.
function initializeDatabase(db) {
  const store = db.createObjectStore('store', {autoIncrement: true});
  store.createIndex('index', /*keypath=*/ 'value');
  store.put({value: 'z'});
  store.put({value: 'y'});
  store.put({value: 'x'});
  store.put({value: 'w'});
}

function isIndex(cursorSourceType) {
  return cursorSourceType === 'IDBIndex';
}

// Return the object store or index, depending on the test's `cursorSourceType`.
function getCursorSource(transaction, cursorSourceType) {
  let cursorSource = transaction.objectStore('store');
  if (isIndex(cursorSourceType)) {
    cursorSource = cursorSource.index('index');
  }
  return cursorSource;
}

// Verify the request source after calling delete() or update() on the cursor.
function cursor_request_source_test(
    cursorSourceType, createRequestFunctionName, createRequestFunctionArgs) {
  indexeddb_test(
      (t, db) => initializeDatabase(db),
      (t, db) => {
        const tx = db.transaction('store', 'readwrite');
        const cursorSource = getCursorSource(tx, cursorSourceType);

        // Open the cursor.
        const openCursorRequest = cursorSource.openCursor();
        openCursorRequest.onerror =
            t.unreached_func('The cursor must not fail to open.');

        openCursorRequest.onsuccess = t.step_func(e => {
          // Use the cursor to create a new request.
          const cursor = e.target.result;
          const request =
              cursor[createRequestFunctionName](...createRequestFunctionArgs);
          assert_equals(
              request.source, cursor,
              `The request's source must be the cursor itself.`);
          t.done();
        });
      },
      `The source of the request from ${cursorSourceType}::${
          createRequestFunctionName}() is the cursor itself`);
}

// Verify the request source after calling openCursor() or openKeyCursor() and
// then using the cursor to iterate.
function open_cursor_request_source_test(
    cursorSourceType, openCursorFunctionName) {
  indexeddb_test(
      (t, db) => initializeDatabase(db),
      (t, db) => {
        const tx = db.transaction('store', 'readonly');
        const cursorSource = getCursorSource(tx, cursorSourceType);

        // Open the cursor.
        const openCursorRequest = cursorSource[openCursorFunctionName]();
        openCursorRequest.onerror =
            t.unreached_func('The cursor must not fail to open or iterate.');

        assert_equals(
            openCursorRequest.source, cursorSource,
            'The request source must be the opener of the cursor.');

        // Verify the cursor's `request.source` after iterating with
        // `advance()`, `continue()`, and `continuePrimaryKey()`.
        let iterationCount = 0;
        openCursorRequest.onsuccess = t.step_func(e => {
          assert_equals(
              openCursorRequest.source, cursorSource,
              'The request source must be the opener of the cursor after iterating.');

          const cursor = e.target.result;
          ++iterationCount;

          if (iterationCount == 1) {
            cursor.advance(1);
          } else if (iterationCount == 2) {
            cursor.continue();
          } else if (iterationCount == 3 && isIndex(cursorSourceType)) {
            cursor.continuePrimaryKey('z', 0);
          } else {
            t.done();
          }
        });
      },
      `${cursorSourceType}::${
          openCursorFunctionName}'s request source must be the ${
          cursorSourceType} instance that opened the cursor`);
}

open_cursor_request_source_test('IDBObjectStore', 'openCursor');
open_cursor_request_source_test('IDBObjectStore', 'openKeyCursor');
open_cursor_request_source_test('IDBIndex', 'openCursor');
open_cursor_request_source_test('IDBIndex', 'openKeyCursor');

cursor_request_source_test('IDBObjectStore', 'update', /*args=*/[0]);
cursor_request_source_test('IDBObjectStore', 'delete', /*args=*/[]);
cursor_request_source_test('IDBIndex', 'update', /*args=*/[0]);
cursor_request_source_test('IDBIndex', 'delete', /*args=*/[]);
