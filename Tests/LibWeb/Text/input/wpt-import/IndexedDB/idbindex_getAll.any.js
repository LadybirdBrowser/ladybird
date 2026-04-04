// META: title=IndexedDB: Test IDBIndex.getAll
// META: global=window,worker
// META: script=resources/nested-cloning-common.js
// META: script=resources/support.js
// META: script=resources/support-get-all.js
// META: script=resources/support-promises.js
// META: timeout=long
'use strict';


index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {query: 'C'}, 'Single item get');

index_get_all_values_test(
    /*storeName=*/ 'empty', /*options=*/ undefined, 'Empty object store');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ undefined, 'Get all');

index_get_all_values_test(
    /*storeName=*/ 'generated', /*options=*/ undefined,
    'Get all with generated keys');

index_get_all_values_test(
    /*storeName=*/ 'large-values', /*options=*/ undefined,
    'Get all with large values');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 10}, 'maxCount=10');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.bound('G', 'M')}, 'Get bound range');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.bound('G', 'M'), count: 3},
    'Get bound range with maxCount');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {
      query:
          IDBKeyRange.bound('G', 'K', /*lowerOpen=*/ false, /*upperOpen=*/ true)
    },
    'Get upper excluded');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {
      query:
          IDBKeyRange.bound('G', 'K', /*lowerOpen=*/ true, /*upperOpen=*/ false)
    },
    'Get lower excluded');

index_get_all_values_test(
    /*storeName=*/ 'generated',
    /*options=*/ {query: IDBKeyRange.bound(4, 15), count: 3},
    'Get bound range (generated) with maxCount');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {query: 'Doesn\'t exist'},
    'Non existent key');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 0}, 'maxCount=0');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 4294967295},
    'Max value count');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.upperBound('0')},
    'Query with empty range where  first key < upperBound');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.lowerBound('ZZ')},
    'Query with empty range where lowerBound < last key');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line-not-unique', /*options=*/ {query: 'first'},
    'Retrieve multiEntry key');

index_get_all_values_test(
    /*storeName=*/ 'out-of-line-multi', /*options=*/ {query: 'vowel'},
    'Retrieve one key multiple values');

get_all_with_invalid_keys_test(
    'getAll', /*storeName=*/ 'out-of-line', /*indexName=*/ 'test_idx',
    /*shouldUseDictionary=*/ false, 'Get all values with invalid query keys');
