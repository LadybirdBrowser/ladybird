'use strict';

// Should be large enough to trigger large value handling in the IndexedDB
// engines that have special code paths for large values.
const wrapThreshold = 128 * 1024;

function populateStore(store) {
  store.put({id: 1, key: 'k1', value: largeValue(wrapThreshold, 1)});
  store.put({id: 2, key: 'k2', value: ['small-2']});
  store.put({id: 3, key: 'k3', value: largeValue(wrapThreshold, 3)});
  store.put({id: 4, key: 'k4', value: ['small-4']});
}

// Assigns cursor indexes for operations that require open cursors.
//
// Returns the number of open cursors required to perform all operations.
function assignCursors(operations) {
  return cursorCount;
}

// Opens index cursors for operations that require open cursors.
//
// onsuccess is called if all cursors are opened successfully. Otherwise,
// onerror will be called at least once.
function openCursors(testCase, index, operations, onerror, onsuccess) {
  let pendingCursors = 0;

  for (let operation of operations) {
    const opcode = operation[0];
    const primaryKey = operation[1];
    let request;
    switch (opcode) {
      case 'continue':
        request =
            index.openCursor(IDBKeyRange.lowerBound(`k${primaryKey - 1}`));
        break;
      case 'continue-empty':
        // k4 is the last key in the data set, so calling continue() will get
        // the cursor past the end of the store.
        request = index.openCursor(IDBKeyRange.lowerBound('k4'));
        break;
      default:
        continue;
    }

    operation[2] = request;
    ++pendingCursors;

    request.onsuccess = testCase.step_func(() => {
      --pendingCursors;
      if (!pendingCursors)
        onsuccess();
    });
    request.onerror = testCase.step_func(onerror);
  }

  if (!pendingCursors)
    onsuccess();
}

function doOperation(testCase, store, index, operation, requestId, results) {
  const opcode = operation[0];
  const primaryKey = operation[1];
  const cursor = operation[2];

  return new Promise((resolve, reject) => {
    let request;
    switch (opcode) {
      case 'add':  // Tests returning a primary key.
        request =
            store.add({key: `k${primaryKey}`, value: [`small-${primaryKey}`]});
        break;
      case 'put':  // Tests returning a primary key.
        request =
            store.put({key: `k${primaryKey}`, value: [`small-${primaryKey}`]});
        break;
      case 'put-with-id':  // Tests returning success or a primary key.
        request = store.put({
          key: `k${primaryKey}`,
          value: [`small-${primaryKey}`],
          id: primaryKey
        });
        break;
      case 'get':        // Tests returning a value.
      case 'get-empty':  // Tests returning undefined.
        request = store.get(primaryKey);
        break;
      case 'getall':  // Tests returning an array of values.
        request = store.getAll();
        break;
      case 'error':  // Tests returning an error.
        request =
            store.put({key: `k${primaryKey}`, value: [`small-${primaryKey}`]});
        request.onerror = testCase.step_func(event => {
          event.preventDefault();
          results.push([requestId, request.error]);
          resolve();
        });
        request.onsuccess = testCase.step_func(() => {
          reject(new Error('put with duplicate primary key succeded'));
        });
        break;
      case 'continue':  // Tests returning a key, primary key, and value.
        request = cursor;
        cursor.result.continue();
        request.onsuccess = testCase.step_func(() => {
          const result = request.result;
          results.push(
              [requestId, result.key, result.primaryKey, result.value]);
          resolve();
        });
        request.onerror = null;
        break;
      case 'open':  // Tests returning a cursor, key, primary key, and value.
        request = index.openCursor(IDBKeyRange.lowerBound(`k${primaryKey}`));
        request.onsuccess = testCase.step_func(() => {
          const result = request.result;
          results.push(
              [requestId, result.key, result.primaryKey, result.value]);
          resolve();
        });
        break;
      case 'continue-empty':  // Tests returning a null result.
        request = cursor;
        cursor.result.continue();
        request.onsuccess = testCase.step_func(() => {
          results.push([requestId, request.result]);
          resolve();
        });
        request.onerror = null;
        break;
      case 'open-empty':  // Tests returning a null cursor.
        request = index.openCursor(IDBKeyRange.lowerBound(`k${primaryKey}`));
        request.onsuccess = testCase.step_func(() => {
          const result = request.result;
          results.push([requestId, request.result]);
          resolve();
        });
        break;
      case 'count':  // Tests returning a numeric result.
        request = index.count();
        request.onsuccess = testCase.step_func(() => {
          results.push([requestId, request.result]);
          resolve();
        });
        break;
    };

    if (!request.onsuccess) {
      request.onsuccess = testCase.step_func(() => {
        results.push([requestId, request.result]);
        resolve();
      });
    }
    if (!request.onerror)
      request.onerror = testCase.step_func(event => {
        event.preventDefault();
        reject(request.error);
      });
  });
}

function checkOperationResult(operation, result, requestId) {
  const opcode = operation[0];
  const primaryKey = operation[1];

  const expectedValue = (primaryKey == 1 || primaryKey == 3) ?
      largeValue(wrapThreshold, primaryKey) :
      [`small-${primaryKey}`];

  const requestIndex = result[0];
  assert_equals(
      requestIndex, requestId, 'result event order should match request order');
  switch (opcode) {
    case 'put':
    case 'put-with-id':
    case 'add':
      assert_equals(
          result[1], primaryKey,
          `${opcode} result should be the new object's primary key`);
      break;
    case 'get':
      assert_equals(
          result[1].id, primaryKey,
          'get result should match put value (primary key)');
      assert_equals(
          result[1].key, `k${primaryKey}`,
          'get result should match put value (key)');
      assert_equals(
          result[1].value.join(','), expectedValue.join(','),
          'get result should match put value (nested value)');
      break;
    case 'getall':
      assert_equals(
          result[1].length, primaryKey,
          'getAll should return all the objects in the store');
      for (let i = 0; i < primaryKey; ++i) {
        const object = result[1][i];
        assert_equals(
            object.id, i + 1,
            `getAll result ${i + 1} should match put value (primary key)`);
        assert_equals(
            object.key, `k${i + 1}`,
            `get result ${i + 1} should match put value (key)`);

        const expectedValue = (i == 0 || i == 2) ?
            largeValue(wrapThreshold, i + 1) :
            [`small-${i + 1}`];
        assert_equals(
            object.value.join(','), object.value.join(','),
            `get result ${i + 1} should match put value (nested value)`);
      }
      break;
    case 'get-empty':
      assert_equals(
          result[1], undefined, 'get-empty result should be undefined');
      break;
    case 'error':
      assert_equals(
          result[1].name, 'ConstraintError',
          'incorrect error from put with duplicate primary key');
      break;
    case 'continue':
    case 'open':
      assert_equals(
          result[1], `k${primaryKey}`,
          `${opcode} key should match the key in the put value`);
      assert_equals(
          result[2], primaryKey,
          `${opcode} primary key should match the put value's primary key`);
      assert_equals(
          result[3].id, primaryKey,
          `${opcode} value should match put value (primary key)`);
      assert_equals(
          result[3].key, `k${primaryKey}`,
          `${opcode} value should match put value (key)`);
      assert_equals(
          result[3].value.join(','), expectedValue.join(','),
          `${opcode} value should match put value (nested value)`);
      break;
    case 'continue-empty':
    case 'open-empty':
      assert_equals(result[1], null, `${opcode} result should be null`);
      break;
  }
}

function eventsTest(label, operations) {
  promise_test(testCase => {
    return createDatabase(
               testCase,
               (database, transaction) => {
                 const store = database.createObjectStore(
                     'test-store', {autoIncrement: true, keyPath: 'id'});
                 store.createIndex('test-index', 'key', {unique: true});
                 populateStore(store);
               })
        .then(database => {
          const transaction = database.transaction(['test-store'], 'readwrite');
          const store = transaction.objectStore('test-store');
          const index = store.index('test-index');
          return new Promise((resolve, reject) => {
            openCursors(testCase, index, operations, reject, () => {
              const results = [];
              const promises = [];
              for (let i = 0; i < operations.length; ++i) {
                const promise = doOperation(
                    testCase, store, index, operations[i], i, results);
                promises.push(promise);
              };
              resolve(Promise.all(promises).then(() => results));
            });
          });
        })
        .then(results => {
          assert_equals(
              results.length, operations.length,
              'Promise.all should resolve after all sub-promises resolve');
          for (let i = 0; i < operations.length; ++i)
            checkOperationResult(operations[i], results[i], i);
        });
  }, label);
}
