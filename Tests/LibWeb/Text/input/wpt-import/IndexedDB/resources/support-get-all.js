// META: script=nested-cloning-common.js
// META: script=support.js
// META: script=support-promises.js

'use strict';

// Define constants used to populate object stores and indexes.
const alphabet = 'abcdefghijklmnopqrstuvwxyz'.split('');
const ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('');
const vowels = 'aeiou'.split('');

// Setup the object store identified by `storeName` to test `getAllKeys()`,
// `getAll()` and `getAllRecords()`.
//  - `callback` is a function that runs after setup with the arguments: `test`,
//    `connection`, and `expectedRecords`.
//  - The `expectedRecords` callback argument records all of the keys and values
//    added to the object store during setup.  It is an array of records where
//    each element contains a `key`, `primaryKey` and `value`.  Tests can use
//    `expectedRecords` to verify the actual results from a get all request.
function object_store_get_all_test_setup(storeName, callback, testDescription) {
  const expectedRecords = [];

  indexeddb_test(
      (test, connection) => {
        switch (storeName) {
          case 'generated': {
            // Create an object store with auto-generated, auto-incrementing,
            // inline keys.
            const store = connection.createObjectStore(
                storeName, {autoIncrement: true, keyPath: 'id'});
            alphabet.forEach(letter => {
              store.put({ch: letter});

              const generatedKey = alphabet.indexOf(letter) + 1;
              expectedRecords.push({
                key: generatedKey,
                primaryKey: generatedKey,
                value: {ch: letter}
              });
            });
            return;
          }
          case 'out-of-line': {
            // Create an object store with out-of-line keys.
            const store = connection.createObjectStore(storeName);
            alphabet.forEach(letter => {
              store.put(`value-${letter}`, letter);

              expectedRecords.push(
                  {key: letter, primaryKey: letter, value: `value-${letter}`});
            });
            return;
          }
          case 'empty': {
            // Create an empty object store.
            connection.createObjectStore(storeName);
            return;
          }
          case 'large-values': {
            // Create an object store with 3 large values. `largeValue()`
            // generates the value using the key as the seed.  The keys start at
            // 0 and then increment by 1.
            const store = connection.createObjectStore(storeName);
            for (let i = 0; i < 3; i++) {
              const value = largeValue(/*size=*/ wrapThreshold, /*seed=*/ i);
              store.put(value, i);

              expectedRecords.push({key: i, primaryKey: i, value});
            }
            return;
          }
        }
      },
      // Bind `expectedRecords` to the `indexeddb_test` callback function.
      (test, connection) => {
        callback(test, connection, expectedRecords);
      },
      testDescription);
}

// Similar to `object_store_get_all_test_setup()` above, but also creates an
// index named `test_idx` for each object store.
function index_get_all_test_setup(storeName, callback, testDescription) {
  const expectedRecords = [];

  indexeddb_test(
      function(test, connection) {
        switch (storeName) {
          case 'generated': {
            // Create an object store with auto-incrementing, inline keys.
            // Create an index on the uppercase letter property `upper`.
            const store = connection.createObjectStore(
                storeName, {autoIncrement: true, keyPath: 'id'});
            store.createIndex('test_idx', 'upper');
            alphabet.forEach(function(letter) {
              const value = {ch: letter, upper: letter.toUpperCase()};
              store.put(value);

              const generatedKey = alphabet.indexOf(letter) + 1;
              expectedRecords.push(
                  {key: value.upper, primaryKey: generatedKey, value});
            });
            return;
          }
          case 'out-of-line': {
            // Create an object store with out-of-line keys.  Create an index on
            // the uppercase letter property `upper`.
            const store = connection.createObjectStore(storeName);
            store.createIndex('test_idx', 'upper');
            alphabet.forEach(function(letter) {
              const value = {ch: letter, upper: letter.toUpperCase()};
              store.put(value, letter);

              expectedRecords.push(
                  {key: value.upper, primaryKey: letter, value});
            });
            return;
          }
          case 'out-of-line-not-unique': {
            // Create an index on the `half` property, which is not unique, with
            // two possible values: `first` and `second`.
            const store = connection.createObjectStore(storeName);
            store.createIndex('test_idx', 'half');
            alphabet.forEach(function(letter) {
              let half = 'first';
              if (letter > 'm') {
                half = 'second';
              }

              const value = {ch: letter, half};
              store.put(value, letter);

              expectedRecords.push({key: half, primaryKey: letter, value});
            });
            return
          }
          case 'out-of-line-multi': {
            // Create a multi-entry index on `attribs`, which is an array of
            // strings.
            const store = connection.createObjectStore(storeName);
            store.createIndex('test_idx', 'attribs', {multiEntry: true});
            alphabet.forEach(function(letter) {
              let attrs = [];
              if (['a', 'e', 'i', 'o', 'u'].indexOf(letter) != -1) {
                attrs.push('vowel');
              } else {
                attrs.push('consonant');
              }
              if (letter == 'a') {
                attrs.push('first');
              }
              if (letter == 'z') {
                attrs.push('last');
              }
              const value = {ch: letter, attribs: attrs};
              store.put(value, letter);

              for (let attr of attrs) {
                expectedRecords.push({key: attr, primaryKey: letter, value});
              }
            });
            return;
          }
          case 'empty': {
            // Create an empty index.
            const store = connection.createObjectStore(storeName);
            store.createIndex('test_idx', 'upper');
            return;
          }
          case 'large-values': {
            // Create an object store and index with 3 large values and their
            // seed.  Use the large value's seed as the index key.
            const store = connection.createObjectStore('large-values');
            store.createIndex('test_idx', 'seed');
            for (let i = 0; i < 3; i++) {
              const seed = i;
              const randomValue = largeValue(/*size=*/ wrapThreshold, seed);
              const recordValue = {seed, randomValue};
              store.put(recordValue, i);

              expectedRecords.push(
                  {key: seed, primaryKey: i, value: recordValue});
            }
            return;
          }
          default: {
            test.assert_unreached(`Unknown storeName: ${storeName}`);
          }
        }
      },
      // Bind `expectedRecords` to the `indexeddb_test` callback function.
      (test, connection) => {
        callback(test, connection, expectedRecords);
      },
      testDescription);
}

// Test `getAll()`, `getAllKeys()` or `getAllRecords()` on either `storeName` or
// `optionalIndexName` with the given `options`.
//
//  - `getAllFunctionName` is name of the function to test, which must be
//     `getAll`, `getAllKeys` or `getAllRecords`.
//
//  - `options` is an `IDBGetAllOptions` dictionary that may contain a  `query`,
//    `direction` and `count`.
//
// - `shouldUseDictionaryArgument` is true when testing the get all function
//    overloads that takes an `IDBGetAllOptions` dictionary.  False tests the
//    overloads that take two optional arguments: `query` and `count`.
function get_all_test(
    getAllFunctionName, storeName, optionalIndexName, options,
    shouldUseDictionaryArgument, testDescription) {
  const testGetAllCallback = (test, connection, expectedRecords) => {
    // Create a transaction and a get all request.
    const transaction = connection.transaction(storeName, 'readonly');
    let queryTarget = transaction.objectStore(storeName);
    if (optionalIndexName) {
      queryTarget = queryTarget.index(optionalIndexName);
    }
    const request = createGetAllRequest(
        getAllFunctionName, queryTarget, options, shouldUseDictionaryArgument);
    request.onerror = test.unreached_func('The get all request must succeed');

    // Verify the results after the get all request completes.
    request.onsuccess = test.step_func(event => {
      const actualResults = event.target.result;
      const expectedResults = calculateExpectedGetAllResults(
          getAllFunctionName, expectedRecords, options);
      verifyGetAllResults(getAllFunctionName, actualResults, expectedResults);
      test.done();
    });
  };

  if (optionalIndexName) {
    index_get_all_test_setup(storeName, testGetAllCallback, testDescription);
  } else {
    object_store_get_all_test_setup(
        storeName, testGetAllCallback, testDescription);
  }
}

function object_store_get_all_keys_test(storeName, options, testDescription) {
  get_all_test(
      'getAllKeys', storeName, /*indexName=*/ undefined, options,
      /*shouldUseDictionaryArgument=*/ false, testDescription);
}

function object_store_get_all_values_test(storeName, options, testDescription) {
  get_all_test(
      'getAll', storeName, /*indexName=*/ undefined, options,
      /*shouldUseDictionaryArgument=*/ false, testDescription);
}

function object_store_get_all_values_with_options_test(
    storeName, options, testDescription) {
  get_all_test(
      'getAll', storeName, /*indexName=*/ undefined, options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function object_store_get_all_keys_with_options_test(
    storeName, options, testDescription) {
  get_all_test(
      'getAllKeys', storeName, /*indexName=*/ undefined, options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function object_store_get_all_records_test(
    storeName, options, testDescription) {
  get_all_test(
      'getAllRecords', storeName, /*indexName=*/ undefined, options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function index_get_all_keys_test(storeName, options, testDescription) {
  get_all_test(
      'getAllKeys', storeName, 'test_idx', options,
      /*shouldUseDictionaryArgument=*/ false, testDescription);
}

function index_get_all_keys_with_options_test(
    storeName, options, testDescription) {
  get_all_test(
      'getAllKeys', storeName, 'test_idx', options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function index_get_all_values_test(storeName, options, testDescription) {
  get_all_test(
      'getAll', storeName, 'test_idx', options,
      /*shouldUseDictionaryArgument=*/ false, testDescription);
}

function index_get_all_values_with_options_test(
    storeName, options, testDescription) {
  get_all_test(
      'getAll', storeName, 'test_idx', options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function index_get_all_records_test(storeName, options, testDescription) {
  get_all_test(
      'getAllRecords', storeName, 'test_idx', options,
      /*shouldUseDictionaryArgument=*/ true, testDescription);
}

function createGetAllRequest(
    getAllFunctionName, queryTarget, options, shouldUseDictionaryArgument) {
  if (options && shouldUseDictionaryArgument) {
    assert_true(
        'getAllRecords' in queryTarget,
        `"${queryTarget}" must support "getAllRecords()" to use an "IDBGetAllOptions" dictionary with "${
            getAllFunctionName}".`);
    return queryTarget[getAllFunctionName](options);
  }
  // `getAll()` and `getAllKeys()` use optional arguments.  Omit the
  // optional arguments when undefined.
  if (options && options.count) {
    return queryTarget[getAllFunctionName](options.query, options.count);
  }
  if (options && options.query) {
    return queryTarget[getAllFunctionName](options.query);
  }
  return queryTarget[getAllFunctionName]();
}

// Returns the expected results when `getAllFunctionName` is called with
// `options` to query an object store or index containing `records`.
function calculateExpectedGetAllResults(getAllFunctionName, records, options) {
  const expectedRecords = filterWithGetAllRecordsOptions(records, options);
  switch (getAllFunctionName) {
    case 'getAll':
      return expectedRecords.map(({value}) => {return value});
    case 'getAllKeys':
      return expectedRecords.map(({primaryKey}) => {return primaryKey});
    case 'getAllRecords':
      return expectedRecords;
  }
  assert_unreached(`Unknown getAllFunctionName: "${getAllFunctionName}"`);
}

// Asserts that the array of results from `getAllFunctionName` matches the
// expected results.
function verifyGetAllResults(getAllFunctionName, actual, expected) {
  switch (getAllFunctionName) {
    case 'getAll':
      assert_idb_values_equals(actual, expected);
      return;
    case 'getAllKeys':
      assert_array_equals(actual, expected);
      return;
    case 'getAllRecords':
      assert_records_equals(actual, expected);
      return;
  }
  assert_unreached(`Unknown getAllFunctionName: "${getAllFunctionName}"`);
}

// Returns the array of `records` that satisfy `options`.  Tests may use this to
// generate expected results.
//  - `records` is an array of objects where each object has the properties:
//    `key`, `primaryKey`, and `value`.
//  - `options` is an `IDBGetAllRecordsOptions ` dictionary that may contain a
//    `query`, `direction` and `count`.
function filterWithGetAllRecordsOptions(records, options) {
  if (!options) {
    return records;
  }

  // Remove records that don't satisfy the query.
  if (options.query) {
    let query = options.query;
    if (!(query instanceof IDBKeyRange)) {
      // Create an IDBKeyRange for the query's key value.
      query = IDBKeyRange.only(query);
    }
    records = records.filter(record => query.includes(record.key));
  }

  // Remove duplicate records.
  if (options.direction === 'nextunique' ||
      options.direction === 'prevunique') {
    const uniqueRecords = [];
    records.forEach(record => {
      if (!uniqueRecords.some(
              unique => IDBKeyRange.only(unique.key).includes(record.key))) {
        uniqueRecords.push(record);
      }
    });
    records = uniqueRecords;
  }

  // Reverse the order of the records.
  if (options.direction === 'prev' || options.direction === 'prevunique') {
    records = records.slice().reverse();
  }

  // Limit the number of records.
  if (options.count) {
    records = records.slice(0, options.count);
  }
  return records;
}

function isArrayOrArrayBufferView(value) {
  return Array.isArray(value) || ArrayBuffer.isView(value);
}

// This function compares the string representation of the arrays because
// `assert_array_equals()` is too slow for large values.
function assert_large_array_equals(actual, expected, description) {
  const array_string = actual.join(',');
  const expected_string = expected.join(',');
  assert_equals(array_string, expected_string, description);
}

// Verifies two IDB values are equal.  The expected value may be a primitive, an
// object, or an array.
function assert_idb_value_equals(actual_value, expected_value) {
  if (isArrayOrArrayBufferView(expected_value)) {
    assert_large_array_equals(
        actual_value, expected_value,
        'The record must have the expected value');
  } else if (typeof expected_value === 'object') {
    // Verify each property of the object value.
    for (let property_name of Object.keys(expected_value)) {
      if (isArrayOrArrayBufferView(expected_value[property_name])) {
        // Verify the array property value.
        assert_large_array_equals(
            actual_value[property_name], expected_value[property_name],
            `The record must contain the array value "${
                JSON.stringify(
                    expected_value)}" with property "${property_name}"`);
      } else {
        // Verify the primitive property value.
        assert_equals(
            actual_value[property_name], expected_value[property_name],
            `The record must contain the value "${
                JSON.stringify(
                    expected_value)}" with property "${property_name}"`);
      }
    }
  } else {
    // Verify the primitive value.
    assert_equals(
        actual_value, expected_value,
        'The record must have the expected value');
  }
}

// Verifies each record from the results of `getAllRecords()`.
function assert_record_equals(actual_record, expected_record) {
  assert_class_string(
      actual_record, 'IDBRecord', 'The record must be an IDBRecord');
  assert_idl_attribute(
      actual_record, 'key', 'The record must have a key attribute');
  assert_idl_attribute(
      actual_record, 'primaryKey',
      'The record must have a primaryKey attribute');
  assert_idl_attribute(
      actual_record, 'value', 'The record must have a value attribute');

  // Verify the attributes: `key`, `primaryKey` and `value`.
  assert_equals(
      actual_record.primaryKey, expected_record.primaryKey,
      'The record must have the expected primaryKey');
  assert_equals(
      actual_record.key, expected_record.key,
      'The record must have the expected key');
  assert_idb_value_equals(actual_record.value, expected_record.value);
}

// Verifies the results from `getAllRecords()`, which is an array of records:
// [
//   { 'key': key1, 'primaryKey': primary_key1, 'value': value1 },
//   { 'key': key2, 'primaryKey': primary_key2, 'value': value2 },
//   ...
// ]
function assert_records_equals(actual_records, expected_records) {
  assert_true(
      Array.isArray(actual_records),
      'The records must be an array of IDBRecords');
  assert_equals(
      actual_records.length, expected_records.length,
      'The records array must contain the expected number of records');

  for (let i = 0; i < actual_records.length; i++) {
    assert_record_equals(actual_records[i], expected_records[i]);
  }
}

// Verifies the results from `getAll()`, which is an array of IndexedDB record
// values.
function assert_idb_values_equals(actual_values, expected_values) {
  assert_true(Array.isArray(actual_values), 'The values must be an array');
  assert_equals(
      actual_values.length, expected_values.length,
      'The values array must contain the expected number of values');

  for (let i = 0; i < actual_values.length; i++) {
    assert_idb_value_equals(actual_values[i], expected_values[i]);
  }
}

// Test passing both an options dictionary  and a count to `getAll()` and
// `getAllKeys()`.  The get all request must ignore the `count` argument, using
//  count from the options dictionary instead.
function get_all_with_options_and_count_test(
    getAllFunctionName, storeName, optionalIndexName, testDescription) {
  // Set up the object store or index to query.
  const setupFunction = optionalIndexName ? index_get_all_test_setup :
                                            object_store_get_all_test_setup;

  setupFunction(storeName, (test, connection, expectedRecords) => {
    const transaction = connection.transaction(storeName, 'readonly');
    let queryTarget = transaction.objectStore(storeName);
    if (optionalIndexName) {
      queryTarget = queryTarget.index(optionalIndexName);
    }

    const options = {count: 10};
    const request = queryTarget[getAllFunctionName](options, /*count=*/ 17);

    request.onerror =
        test.unreached_func(`"${getAllFunctionName}()" request must succeed.`);

    request.onsuccess = test.step_func(event => {
      const expectedResults = calculateExpectedGetAllResults(
          getAllFunctionName, expectedRecords, options);

      const actualResults = event.target.result;
      verifyGetAllResults(getAllFunctionName, actualResults, expectedResults);

      test.done();
    });
  }, testDescription);
}

// Get all operations must throw a `DataError` exception for invalid query keys.
// See `get_all_test()` above for a description of the parameters.
function get_all_with_invalid_keys_test(
    getAllFunctionName, storeName, optionalIndexName,
    shouldUseDictionaryArgument, testDescription) {
  // Set up the object store or index to query.
  const setupFunction = optionalIndexName ? index_get_all_test_setup :
                                            object_store_get_all_test_setup;

  setupFunction(storeName, (test, connection, expectedRecords) => {
    const transaction = connection.transaction(storeName, 'readonly');
    let queryTarget = transaction.objectStore(storeName);
    if (optionalIndexName) {
      queryTarget = queryTarget.index(optionalIndexName);
    }

    const invalidKeys = [
      {
        description: 'Date(NaN)',
        value: new Date(NaN),
      },
      {
        description: 'Array',
        value: [{}],
      },
      {
        description: 'detached TypedArray',
        value: createDetachedArrayBuffer(),
      },
      {
        description: 'detached ArrayBuffer',
        value: createDetachedArrayBuffer().buffer
      },
    ];
    invalidKeys.forEach(({description, value}) => {
      const argument = shouldUseDictionaryArgument ? {query: value} : value;
      assert_throws_dom('DataError', () => {
        queryTarget[getAllFunctionName](argument);
      }, `An invalid ${description} key must throw an exception.`);
    });
    test.done();
  }, testDescription);
}
