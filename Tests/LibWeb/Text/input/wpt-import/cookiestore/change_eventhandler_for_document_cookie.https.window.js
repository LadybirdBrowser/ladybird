// META: title=Cookie Store API: Observing 'change' events in document when cookies set via document.cookie
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-cookie=value; path=/');
  assert_equals(
      await getCookieString(),
      'DOCUMENT-cookie=value',
      'Cookie we wrote using document.cookie in cookie jar');
  assert_equals(
    await getCookieStringHttp(),
    'DOCUMENT-cookie=value',
    'Cookie we wrote using document.cookie in HTTP cookie jar');
  assert_equals(
      await getCookieStringDocument(),
      'DOCUMENT-cookie=value',
      'Cookie we wrote using document.cookie in document.cookie');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-cookie', value: 'value'}]},
      'Cookie we wrote using document.cookie is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-cookie=new-value; path=/');
  assert_equals(
      await getCookieString(),
      'DOCUMENT-cookie=new-value',
      'Cookie we overwrote using document.cookie in cookie jar');
  assert_equals(
    await getCookieStringHttp(),
    'DOCUMENT-cookie=new-value',
    'Cookie we overwrote using document.cookie in HTTP cookie jar');
  assert_equals(
      await getCookieStringDocument(),
      'DOCUMENT-cookie=new-value',
      'Cookie we overwrote using document.cookie in document.cookie');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-cookie', value: 'new-value'}]},
      'Cookie we overwrote using document.cookie is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-cookie=DELETED; path=/; max-age=0');
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after document.cookie' +
        ' cookie-clearing using max-age=0');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'Empty HTTP cookie jar after document.cookie' +
        ' cookie-clearing using max-age=0');
  assert_equals(
      await getCookieStringDocument(),
      undefined,
      'Empty document.cookie cookie jar after document.cookie' +
        ' cookie-clearing using max-age=0');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'DOCUMENT-cookie'}]},
      'Deletion observed after document.cookie cookie-clearing' +
        ' using max-age=0');
}, 'document.cookie set/overwrite/delete observed by CookieStore');


cookie_test(async t => {
  const eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-cookie=ALREADY-EXPIRED; path=/; max-age=0');
  await setCookieStringDocument('DOCUMENT-alt-cookie=IGNORE; path=/; max-age=10');
  assert_equals(
    await getCookieStringDocument(),
    'DOCUMENT-alt-cookie=IGNORE',
    'Already-expired cookie not included in document.cookie');
  await verifyCookieChangeEvent(
    eventPromise,
    {deleted: [], changed: [{name: 'DOCUMENT-alt-cookie', value: 'IGNORE'}]},
    'Deletion not observed after document.cookie sets already-expired cookie');
}, 'document.cookie set already-expired cookie should not be observed by CookieStore');

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-cookie=VALUE; path=/');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-cookie', value: 'VALUE'}]},
      'Original cookie is observed.');

  eventPromise = observeNextCookieChangeEvent();
  // Overwrite the original cookie with a duplicate, this should not dispatch an event.
  await setCookieStringDocument('DOCUMENT-cookie=VALUE; path=/');
  await setCookieStringDocument('DOCUMENT-alt-cookie=IGNORE; path=/');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-alt-cookie', value: 'IGNORE'}]},
    'Duplicate cookie is not observed.');
}, 'document.cookie duplicate cookie should not be observed by CookieStore');

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('DOCUMENT-cookie', 'value');
  assert_equals(
      await getCookieString(),
      'DOCUMENT-cookie=value',
      'Cookie we wrote using CookieStore in cookie jar');
  assert_equals(
    await getCookieStringHttp(),
    'DOCUMENT-cookie=value',
    'Cookie we wrote using CookieStore in HTTP cookie jar');
  assert_equals(
      await getCookieStringDocument(),
      'DOCUMENT-cookie=value',
      'Cookie we wrote using CookieStore in document.cookie');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-cookie', value: 'value'}]},
      'Cookie we wrote using CookieStore is observed');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('DOCUMENT-cookie', 'new-value');
  assert_equals(
      await getCookieString(),
      'DOCUMENT-cookie=new-value',
      'Cookie we overwrote using CookieStore in cookie jar');
  assert_equals(
    await getCookieStringHttp(),
    'DOCUMENT-cookie=new-value',
    'Cookie we overwrote using CookieStore in HTTP cookie jar');
  assert_equals(
      await getCookieStringDocument(),
      'DOCUMENT-cookie=new-value',
      'Cookie we overwrote using CookieStore in document.cookie');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-cookie', value: 'new-value'}]},
      'Cookie we overwrote using CookieStore is observed');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.delete('DOCUMENT-cookie');
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after CookieStore delete');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'Empty HTTP cookie jar after CookieStore delete');
  assert_equals(
      await getCookieStringDocument(),
      undefined,
      'Empty document.cookie cookie jar after CookieStore delete');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'DOCUMENT-cookie'}]},
      'Deletion observed after CookieStore delete');
}, 'CookieStore set/overwrite/delete observed by document.cookie');


cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-🍪=🔵; path=/');
  assert_equals(
      await getCookieString(),
      'DOCUMENT-🍪=🔵',
      'Cookie we wrote using document.cookie in cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'DOCUMENT-🍪', value: '🔵'}]},
      'Cookie we wrote using document.cookie is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('DOCUMENT-🍪=DELETED; path=/; max-age=0');
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after document.cookie' +
        ' cookie-clearing using max-age=0');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'DOCUMENT-🍪'}]},
      'Deletion observed after document.cookie cookie-clearing' +
        ' using max-age=0');
}, 'CookieStore agrees with document.cookie on encoding non-ASCII cookies');


cookie_test(async t => {
  await cookieStore.set('DOCUMENT-🍪', '🔵');
  assert_equals(
      await getCookieStringDocument(),
      'DOCUMENT-🍪=🔵',
      'Cookie we wrote using CookieStore in document.cookie');

  await cookieStore.delete('DOCUMENT-🍪');
  assert_equals(
      await getCookieStringDocument(),
      undefined,
      'Empty cookie jar after CookieStore delete');
}, 'document.cookie agrees with CookieStore on encoding non-ASCII cookies');
