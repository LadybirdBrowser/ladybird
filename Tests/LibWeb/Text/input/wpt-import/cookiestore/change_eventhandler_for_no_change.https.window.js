// META: title=Cookie Store API: Test that setting a duplicate cookie does not fire a second event.
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('cookie', 'VALUE');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Original cookie is observed.');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('cookie', 'VALUE');
  await cookieStore.set('alt-cookie', 'IGNORE');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'alt-cookie', value: 'IGNORE'}]},
    'Duplicate cookie is not observed.');
}, 'CookieStore duplicate cookie should not be observed');

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    partitioned: true,
  });
  await verifyCookieChangeEvent(
    eventPromise,
    {changed: [{name: 'cookie', value: 'VALUE', partitioned: true}]},
    'Original cookie is observed.');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    partitioned: true,
  });
  await cookieStore.set('alt-cookie', 'IGNORE');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'alt-cookie', value: 'IGNORE'}]},
    'Duplicate cookie is not observed.');
}, 'CookieStore duplicate partitioned cookie should not be observed');
