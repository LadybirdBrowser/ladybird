// META: global=window,worker
// META: title=Key sort order
// META: script=resources/support.js

// Spec: https://w3c.github.io/IndexedDB/#key-construct

'use strict';

const global_db = createdb_for_multiple_tests();

const keysort = (desc, unsorted, expected) => {
  async_test(t => {
    const store_name = 'store-' + Date.now() + Math.random();

    // The database test
    const open_rq = global_db.setTest(t);
    open_rq.onupgradeneeded = t.step_func(e => {
      const db = e.target.result;
      const objStore = db.createObjectStore(store_name);

      for (let i = 0; i < unsorted.length; i++)
        objStore.add('value', unsorted[i]);
    });

    open_rq.onsuccess = t.step_func(e => {
      const db = e.target.result;
      const actual_keys = [];
      const rq =
          db.transaction(store_name).objectStore(store_name).openCursor();

      rq.onsuccess = t.step_func(e => {
        const cursor = e.target.result;

        if (cursor) {
          actual_keys.push(cursor.key);
          cursor.continue();
        } else {
          assert_key_equals(actual_keys, expected, 'keyorder array');
          assert_equals(actual_keys.length, expected.length, 'array length');

          t.done();
        }
      });
    });
  }, `Database readback sort - ${desc}`);

  // The IDBKey.cmp test
  test(() => {
    const sorted = unsorted.slice(0).sort((a, b) => indexedDB.cmp(a, b));
    assert_key_equals(sorted, expected, 'sorted array');
  }, `IDBKey.cmp sort - ${desc}`);
};

const now = new Date();
const one_sec_ago = new Date(now - 1000);
const one_min_future = new Date(now.getTime() + 1000 * 60);

keysort('String < Array', [[0], 'yo', '', []], ['', 'yo', [], [0]]);

keysort(
    'float < String', [Infinity, 'yo', 0, '', 100],
    [0, 100, Infinity, '', 'yo']);

keysort(
    'float < Date', [now, 0, 9999999999999, -0.22],
    [-0.22, 0, 9999999999999, now]);

keysort(
    'float < Date < String < Array', [[], '', now, [0], '-1', 0, 9999999999999],
    [0, 9999999999999, now, '', '-1', [], [0]]);

keysort(
    'Date(1 sec ago) < Date(now) < Date(1 minute in future)',
    [now, one_sec_ago, one_min_future], [one_sec_ago, now, one_min_future]);

keysort(
    '-1.1 < 1 < 1.01337 < 1.013373 < 2', [1.013373, 2, 1.01337, -1.1, 1],
    [-1.1, 1, 1.01337, 1.013373, 2]);

keysort(
    '-Infinity < -0.01 < 0 < Infinity', [0, -0.01, -Infinity, Infinity],
    [-Infinity, -0.01, 0, Infinity]);

keysort(
    '"" < "a" < "ab" < "b" < "ba"', ['a', 'ba', '', 'b', 'ab'],
    ['', 'a', 'ab', 'b', 'ba']);

keysort(
    'Arrays', [[[0]], [0], [], [0, 0], [0, [0]]],
    [[], [0], [0, 0], [0, [0]], [[0]]]);

const big_array = [];
const bigger_array = [];
for (let i = 0; i < 10000; i++) {
  big_array.push(i);
  bigger_array.push(i);
}
bigger_array.push(0);

keysort(
    'Array.length: 10,000 < Array.length: 10,001',
    [bigger_array, [0, 2, 3], [0], [9], big_array],
    [[0], big_array, bigger_array, [0, 2, 3], [9]]);

keysort(
    'Infinity inside arrays',
    [
      [Infinity, 1],
      [Infinity, Infinity],
      [1, 1],
      [1, Infinity],
      [1, -Infinity],
      [-Infinity, Infinity],
    ],
    [
      [-Infinity, Infinity],
      [1, -Infinity],
      [1, 1],
      [1, Infinity],
      [Infinity, 1],
      [Infinity, Infinity],
    ]);

keysort(
    'Test different stuff at once',
    [
      now,
      [0, []],
      'test',
      1,
      ['a', [1, [-1]]],
      ['b', 'a'],
      [0, 2, 'c'],
      ['a', [1, 2]],
      [],
      [0, [], 3],
      ['a', 'b'],
      [1, 2],
      ['a', 'b', 'c'],
      one_sec_ago,
      [0, 'b', 'c'],
      Infinity,
      -Infinity,
      2.55,
      [0, now],
      [1],
    ],
    [
      -Infinity,
      1,
      2.55,
      Infinity,
      one_sec_ago,
      now,
      'test',
      [],
      [0, 2, 'c'],
      [0, now],
      [0, 'b', 'c'],
      [0, []],
      [0, [], 3],
      [1],
      [1, 2],
      ['a', 'b'],
      ['a', 'b', 'c'],
      ['a', [1, 2]],
      ['a', [1, [-1]]],
      ['b', 'a'],
    ]);
