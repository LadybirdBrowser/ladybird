// META: title=IndexedDB: request result events are delivered in order
// META: global=window,worker
// META: script=resources/support-promises.js
// META: script=resources/support.js
// META: script=resources/request-event-ordering-common.js
// META: timeout=long

// Spec: https://w3c.github.io/IndexedDB/#abort-transaction

'use strict';

eventsTest('small values', [
  ['get', 2],       ['count', 4],       ['continue-empty', null],
  ['get-empty', 5], ['add', 5],         ['open', 2],
  ['continue', 2],  ['get', 4],         ['get-empty', 6],
  ['count', 5],     ['put-with-id', 5], ['put', 6],
  ['error', 3],     ['continue', 4],    ['count', 6],
  ['get-empty', 7], ['open', 4],        ['open-empty', 7],
  ['add', 7],
]);
