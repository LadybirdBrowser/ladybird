'use strict';

// Should be large enough to trigger large value handling in the IndexedDB
// engines that have special code paths for large values.
const wrapThreshold = 128 * 1024;

// Returns an IndexedDB value created from a descriptor.
//
// See the bottom of the file for descriptor samples.
function createValue(descriptor) {
  if (typeof(descriptor) != 'object')
    return descriptor;

  if (Array.isArray(descriptor))
    return descriptor.map((element) => createValue(element));

  if (!descriptor.hasOwnProperty('type')) {
    const value = {};
    for (let property of Object.getOwnPropertyNames(descriptor))
      value[property] = createValue(descriptor[property]);
    return value;
  }

  switch (descriptor.type) {
    case 'blob':
      return new Blob(
          [largeValue(descriptor.size, descriptor.seed)],
          { type: descriptor.mimeType });
    case 'buffer':
      return largeValue(descriptor.size, descriptor.seed);
  }
}

// Checks an IndexedDB value against a descriptor.
//
// Returns a Promise that resolves if the value passes the check.
//
// See the bottom of the file for descriptor samples.
function checkValue(testCase, value, descriptor) {
  if (typeof(descriptor) != 'object') {
    assert_equals(
        descriptor, value,
        'IndexedDB result should match put() argument');
    return Promise.resolve();
  }

  if (Array.isArray(descriptor)) {
    assert_true(
        Array.isArray(value),
        'IndexedDB result type should match put() argument');
    assert_equals(
        descriptor.length, value.length,
        'IndexedDB result array size should match put() argument');

    const subChecks = [];
    for (let i = 0; i < descriptor.length; ++i)
      subChecks.push(checkValue(testCase, value[i], descriptor[i]));
    return Promise.all(subChecks);
  }

  if (!descriptor.hasOwnProperty('type')) {
    assert_array_equals(
        Object.getOwnPropertyNames(value).sort(),
        Object.getOwnPropertyNames(descriptor).sort(),
        'IndexedDB result object properties should match put() argument');
    const subChecks = [];
    return Promise.all(Object.getOwnPropertyNames(descriptor).map(property =>
        checkValue(testCase, value[property], descriptor[property])));
  }

  switch (descriptor.type) {
    case 'blob':
      assert_class_string(
          value, 'Blob',
          'IndexedDB result class should match put() argument');
      assert_equals(
          descriptor.mimeType, value.type,
          'IndexedDB result Blob MIME type should match put() argument');
      assert_equals(descriptor.size, value.size, 'incorrect Blob size');
      return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onloadend = testCase.step_func(() => {
          if (reader.error) {
            reject(reader.error);
            return;
          }
          const view = new Uint8Array(reader.result);
          assert_equals(
              view.join(','),
              largeValue(descriptor.size, descriptor.seed).join(','),
              'IndexedDB result Blob content should match put() argument');
          resolve();
        });
        reader.readAsArrayBuffer(value);
      });

    case 'buffer':
      assert_class_string(
          value, 'Uint8Array',
          'IndexedDB result type should match put() argument');
      assert_equals(
          value.join(','),
          largeValue(descriptor.size, descriptor.seed).join(','),
          'IndexedDB result typed array content should match put() argument');
      return Promise.resolve();
  }
}

function cloningTestInternal(label, valueDescriptors, options) {
  promise_test(testCase => {
    return createDatabase(testCase, (database, transaction) => {
      testCase.add_cleanup(() => database.close());
      let store;
      if (options.useKeyGenerator) {
        store = database.createObjectStore(
            'test-store', { keyPath: 'primaryKey', autoIncrement: true });
      } else {
        store = database.createObjectStore('test-store');
      }
      for (let i = 0; i < valueDescriptors.length; ++i) {
        if (options.useKeyGenerator) {
          store.put(createValue(valueDescriptors[i]));
        } else {
          store.put(createValue(valueDescriptors[i]), i + 1);
        }
      }
    }).then(database => {
      const transaction = database.transaction(['test-store'], 'readonly');
      const store = transaction.objectStore('test-store');
      const subChecks = [];
      let resultIndex = 0;
      for (let i = 0; i < valueDescriptors.length; ++i) {
        subChecks.push(new Promise((resolve, reject) => {
          const requestIndex = i;
          const primaryKey = requestIndex + 1;
          const request = store.get(primaryKey);
          request.onerror =
              testCase.step_func(() => { reject(request.error); });
          request.onsuccess = testCase.step_func(() => {
            assert_equals(
                resultIndex, requestIndex,
                'IDBRequest success events should be fired in request order');
            ++resultIndex;

            const result = request.result;
            if (options.useKeyGenerator) {
              assert_equals(
                  result.primaryKey, primaryKey,
                  'IndexedDB result should have auto-incremented primary key');
              delete result.primaryKey;
            }
            resolve(checkValue(
                testCase, result, valueDescriptors[requestIndex]));
          });
        }));
      }

      subChecks.push(new Promise((resolve, reject) => {
        const requestIndex = valueDescriptors.length;
        const request = store.getAll();
        request.onerror =
            testCase.step_func(() => { reject(request.error); });
        request.onsuccess = testCase.step_func(() => {
          assert_equals(
              resultIndex, requestIndex,
              'IDBRequest success events should be fired in request order');
          ++resultIndex;
          const result = request.result;
          if (options.useKeyGenerator) {
            for (let i = 0; i < valueDescriptors.length; ++i) {
              const primaryKey = i + 1;
              assert_equals(
                  result[i].primaryKey, primaryKey,
                  'IndexedDB result should have auto-incremented primary key');
              delete result[i].primaryKey;
            }
          }
          resolve(checkValue(testCase, result, valueDescriptors));
        });
      }));

      return Promise.all(subChecks);
    });
  }, label);
}

// Performs a series of put()s and verifies that get()s and getAll() match.
//
// Each element of the valueDescriptors array is fed into createValue(), and the
// resulting value is written to IndexedDB via a put() request. After the writes
// complete, the values are read in the same order in which they were written.
// Last, all the results are read one more time via a getAll().
//
// The test verifies that the get() / getAll() results match the arguments to
// put() and that the order in which the get() result events are fired matches
// the order of the get() requests.
function cloningTest(label, valueDescriptors) {
  cloningTestInternal(label, valueDescriptors, { useKeyGenerator: false });
}

// cloningTest, with coverage for key generators.
//
// This creates two tests. One test performs a series of put()s and verifies
// that get()s and getAll() match, exactly like cloningTestWithoutKeyGenerator.
// The other test performs the same put()s in an object store with a key
// generator, and checks that the key generator works properly.
function cloningTestWithKeyGenerator(label, valueDescriptors) {
  cloningTestInternal(label, valueDescriptors, { useKeyGenerator: false });
  cloningTestInternal(
      label + " with key generator", valueDescriptors,
      { useKeyGenerator: true });
}
