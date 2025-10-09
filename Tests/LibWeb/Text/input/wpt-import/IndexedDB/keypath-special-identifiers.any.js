// META: global=window,worker
// META: title=IndexedDB: Special-cased identifiers in extracting keys from values (ES bindings)
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#extract-key-from-value

'use strict';

[{
  type: 'String',
  property: 'length',
  instance: 'abc',
},
 {
   type: 'Array',
   property: 'length',
   instance: ['a', 'b', 'c'],
 },
 {
   type: 'Blob',
   property: 'size',
   instance: new Blob(['abc']),
 },
 {
   type: 'Blob',
   property: 'type',
   instance: new Blob([''], {type: 'foo/bar'}),
 },
 {
   type: 'File',
   property: 'name',
   instance: new File([''], 'foo'),
 },
 {
   type: 'File',
   property: 'lastModified',
   instance: new File([''], '', {lastModified: 123}),
 },
].forEach(function(testcase) {
  indexeddb_test(
      (t, db) => {
        db.createObjectStore(
            'store', {autoIncrement: true, keyPath: testcase.property});
      },
      (t, db) => {
        const key = testcase.instance[testcase.property];
        const tx =
            db.transaction('store', 'readwrite', {durability: 'relaxed'});
        tx.objectStore('store').put(testcase.instance);
        const request = tx.objectStore('store').get(key);
        request.onerror = t.unreached_func('request should not fail');
        request.onsuccess = t.step_func(function() {
          const result = request.result;
          assert_key_equals(
              result[testcase.property], key, 'Property should be used as key');
        });
        tx.oncomplete = t.step_func(function() {
          t.done();
        });
      },
      'Type: ' + testcase.type + ', identifier: ' + testcase.property);
});
