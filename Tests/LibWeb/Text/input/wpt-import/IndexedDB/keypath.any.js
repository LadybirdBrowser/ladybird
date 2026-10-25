// META: global=window,worker
// META: title=Keypath
// META: script=resources/support.js

// Spec:
// http://dvcs.w3.org/hg/IndexedDB/raw-file/tip/Overview.html#key-path-construct

'use strict';

let global_db = createdb_for_multiple_tests();

function keypath(keypath, objects, expected_keys, desc) {
  let db;
  let t = async_test(self.title + ' - ' + (desc ? desc : keypath));
  let store_name = 'store-' + (Date.now()) + Math.random();

  let open_rq = global_db.setTest(t);
  open_rq.onupgradeneeded = function(e) {
    db = e.target.result;
    let objStore = db.createObjectStore(store_name, {keyPath: keypath});

    for (let i = 0; i < objects.length; i++)
      objStore.add(objects[i]);
  };

  open_rq.onsuccess = function(e) {
    let actual_keys = [];
    let rq = db.transaction(store_name).objectStore(store_name).openCursor();

    rq.onsuccess = t.step_func(function(e) {
      let cursor = e.target.result;

      if (cursor) {
        actual_keys.push(cursor.key.valueOf());
        cursor.continue();
      } else {
        assert_key_equals(actual_keys, expected_keys, 'keyorder array');
        t.done();
      }
    });
  };
}

keypath('my.key', [{my: {key: 10}}], [10]);

keypath('my.køi', [{my: {køi: 5}}], [5]);

keypath('my.key_ya', [{my: {key_ya: 10}}], [10]);

keypath('public.key$ya', [{public: {key$ya: 10}}], [10]);

keypath('true.$', [{true: {$: 10}}], [10]);

keypath('my._', [{my: {_: 10}}], [10]);

keypath('delete.a7', [{delete: {a7: 10}}], [10]);

keypath(
    'p.p.p.p.p.p.p.p.p.p.p.p.p.p',
    [{p: {p: {p: {p: {p: {p: {p: {p: {p: {p: {p: {p: {p: {p: 10}}}}}}}}}}}}}}],
    [10]);

keypath(
    'str.length', [{str: 'pony'}, {str: 'my'}, {str: 'little'}, {str: ''}],
    [0, 2, 4, 6]);

keypath(
    'arr.length',
    [
      {arr: [0, 0, 0, 0]}, {arr: [{}, 0, 'hei', 'length', Infinity, []]},
      {arr: [10, 10]}, {arr: []}
    ],
    [0, 2, 4, 6]);

keypath('length', [[10, 10], '123', {length: 20}], [2, 3, 20]);

keypath(
    '', [['bags'], 'bean', 10], [10, 'bean', ['bags']],
    '\'\' uses value as key');

keypath(
    [''], [['bags'], 'bean', 10], [[10], ['bean'], [['bags']]],
    '[\'\'] uses value as [key]');

keypath(
    ['x', 'y'], [{x: 10, y: 20}, {y: 1.337, x: 100}], [[10, 20], [100, 1.337]],
    '[\'x\', \'y\']');

keypath(
    [['x'], ['y']], [{x: 10, y: 20}, {y: 1.337, x: 100}],
    [[10, 20], [100, 1.337]], '[[\'x\'], \'y\'] (stringifies)');

keypath(
    [
      'x', {
        toString: function() {
          return 'y'
        }
      }
    ],
    [{x: 10, y: 20}, {y: 1.337, x: 100}], [[10, 20], [100, 1.337]],
    '[\'x\', {toString->\'y\'}] (stringifies)');

if (false) {
  let myblob = Blob(['Yoda'], {type: 'suprawsum'});
  keypath(
      ['length', 'type'], [myblob], [4, 'suprawsum'],
      '[Blob.length, Blob.type]');
}

// File.name and File.lastModified is not testable automatically

keypath(
    ['name', 'type'],
    [
      {name: 'orange', type: 'fruit'},
      {name: 'orange', type: ['telecom', 'french']}
    ],
    [['orange', 'fruit'], ['orange', ['telecom', 'french']]]);

keypath(
    ['name', 'type.name'],
    [
      {name: 'orange', type: {name: 'fruit'}},
      {name: 'orange', type: {name: 'telecom'}}
    ],
    [['orange', 'fruit'], ['orange', 'telecom']]);

keypath(
    ['type'],
    [{name: 'orange', type: 'fruit'}, {name: 'cucumber', type: 'vegetable'}],
    [['fruit'], ['vegetable']], 'list with 1 field');

// A key path that is a sequence is converted with the WebIDL sequence<DOMString> conversion, which
// stringifies every item. Stringifying a self-referential array recurses without bound, because
// ECMA-262 defines no cycle detection for Array.prototype.join
// (https://tc39.es/ecma262/#sec-array.prototype.join): joining the array converts element 0, which
// is the array itself, which is joined again. The conversion therefore exceeds an implementation
// limit and reports an error. It must not quietly substitute the empty string on the repeat visit,
// which would yield the key path [''] and make each value its own key.
indexeddb_test(
    function(t, db) {
      let loop_array = [];
      loop_array.push(loop_array);
      assert_throws_js(RangeError, function() {
        db.createObjectStore('store', {keyPath: loop_array});
      }, 'Converting a self-referential key path array should exhaust the stack.');
    },
    function(t, db) {
      t.done();
    },
    'Key path conversion: a self-referential array exhausts the stack');
