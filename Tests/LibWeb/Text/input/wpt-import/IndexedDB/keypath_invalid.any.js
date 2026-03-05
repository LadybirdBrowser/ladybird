// META: global=window,worker
// META: title=Invalid keypath
// META: script=resources/support.js

// Spec:
// http://dvcs.w3.org/hg/IndexedDB/raw-file/tip/Overview.html#key-path-construct

'use strict';

let global_db = createdb_for_multiple_tests();

function invalid_keypath(keypath, desc) {
  let t = async_test(
      'Invalid keyPath - ' + (desc ? desc : format_value(keypath)), undefined,
      2);

  let openrq = global_db.setTest(t);
  let store_name = 'store-' + Date.now() + Math.random();

  openrq.onupgradeneeded = function(e) {
    let db = e.target.result;
    assert_throws_dom('SyntaxError', function() {
      db.createObjectStore(store_name, {keyPath: keypath})
    }, 'createObjectStore with keyPath');

    let store = db.createObjectStore(store_name);
    assert_throws_dom('SyntaxError', function() {
      store.createIndex('index', keypath);
    }, 'createIndex with keyPath');

    db.close();

    t.done();
  };
}

invalid_keypath('j a');
invalid_keypath('.yo');
invalid_keypath('yo,lo');
invalid_keypath([]);
invalid_keypath(['array with space']);
invalid_keypath(
    ['multi_array', ['a', 'b']],
    'multidimensional array (invalid toString)');  // => ['multi_array',
                                                   // 'a,b']
invalid_keypath('3m');
invalid_keypath(
    {
      toString: function() {
        return '3m'
      }
    },
    '{toString->3m}');
invalid_keypath('my.1337');
invalid_keypath('..yo');
invalid_keypath('y..o');
invalid_keypath('y.o.');
invalid_keypath('y.o..');
invalid_keypath('m.*');
invalid_keypath('"m"');
invalid_keypath('m%');
invalid_keypath('m/');
invalid_keypath('m/a');
invalid_keypath('m&');
invalid_keypath('m!');
invalid_keypath('*');
invalid_keypath('*.*');
invalid_keypath('^m');
invalid_keypath('/m/');
