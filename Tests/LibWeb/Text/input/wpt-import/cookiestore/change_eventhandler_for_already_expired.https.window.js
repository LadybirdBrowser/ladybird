// META: title=Cookie Store API: Test that setting an already-expired cookie does not trigger an event.
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'ALREADY-EXPIRED',
    expires: new Date(new Date() - 10_000),
  });
  await cookieStore.set('alt-cookie', 'IGNORE');
  assert_equals(
    await getCookieString(),
    'alt-cookie=IGNORE',
    'Already-expired cookie not included in CookieStore');
  await verifyCookieChangeEvent(
    eventPromise,
    {deleted: [], changed: [{name: 'alt-cookie', value: 'IGNORE'}]},
    'Deletion not observed after document.cookie sets already-expired cookie');
}, 'CookieStore setting already-expired cookie should not be observed');

cookie_test(async t => {
  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'ALREADY-EXPIRED',
    expires: new Date(new Date() - 10_000),
    partitioned: true,
  });
  await cookieStore.set('alt-cookie', 'IGNORE');
  assert_equals(
    await getCookieString(),
    'alt-cookie=IGNORE',
    'Already-expired cookie not included in CookieStore');
  await verifyCookieChangeEvent(
    eventPromise,
    {deleted: [], changed: [{name: 'alt-cookie', value: 'IGNORE'}]},
    'Deletion not observed after document.cookie sets already-expired cookie');
}, 'CookieStore setting already-expired partitioned cookie should not be observed');
