// META: title=IndexedDB: Test IDBIndex.getAllRecords
// META: global=window,worker
// META: script=resources/nested-cloning-common.js
// META: script=resources/support.js
// META: script=resources/support-get-all.js
// META: script=resources/support-promises.js
// META: timeout=long
'use strict';


index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {query: /*key=*/ 'C'},
    'Single item');

index_get_all_records_test(
    /*storeName=*/ 'empty', /*options=*/ undefined, 'Empty index');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ undefined, 'Get all records');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {}, 'Get all records with empty options');


index_get_all_records_test(
    /*storeName=*/ 'large-values',
    /*options=*/ undefined, 'Get all records with large value');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 10}, 'Count');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.bound('G', 'M')},
    'Query with bound range');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.bound('G', 'M'), count: 3},
    'Query with bound range and count');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {
      query:
          IDBKeyRange.bound('G', 'M', /*lowerOpen=*/ false, /*upperOpen=*/ true)
    },
    'Query with upper excluded bound range');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {
      query:
          IDBKeyRange.bound('G', 'M', /*lowerOpen=*/ true, /*upperOpen=*/ false)
    },
    'Query with lower excluded bound range');

index_get_all_records_test(
    /*storeName=*/ 'generated',
    /*options=*/ {query: IDBKeyRange.bound(4, 15), count: 3},
    'Query with bound range and count for generated keys');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {query: 'Doesn\'t exist'},
    'Query with Nonexistent key');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 0}, 'Zero count');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {count: 4294967295},
    'Max value count');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.upperBound('0')},
    'Query with empty range where first key < upperBound');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {query: IDBKeyRange.lowerBound('ZZ')},
    'Query with empty range where lowerBound < last key');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line-not-unique', /*options=*/ {query: 'first'},
    'Query index key that matches multiple records');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line-multi', /*options=*/ {query: 'vowel'},
    'Query with multiEntry index');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {direction: 'next'},
    'Direction: next');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {direction: 'prev'},
    'Direction: prev');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line-not-unique',
    /*options=*/ {direction: 'nextunique'}, 'Direction: nextunique');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line-not-unique',
    /*options=*/ {direction: 'prevunique'}, 'Direction: prevunique');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line',
    /*options=*/ {direction: 'prev', query: IDBKeyRange.bound('B', 'X')},
    'Direction and query');

index_get_all_records_test(
    /*storeName=*/ 'out-of-line', /*options=*/ {
      direction: 'prev',
      query: IDBKeyRange.bound('B', 'X'),
      count: 4
    },
    'Direction, query and count');

get_all_with_invalid_keys_test(
    'getAllRecords', /*storeName=*/ 'out-of-line', /*indexName=*/ 'test_idx',
    /*shouldUseDictionary=*/ true, 'Get all records with invalid query keys');
