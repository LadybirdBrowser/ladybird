// META: title=Cookie Store API: Observing 'change' events in document when cookies set via Set-Cookie header
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-cookie=value; path=/');
  assert_equals(
      await getCookieString(),
      'HTTP-cookie=value',
      'Cookie we wrote using HTTP in cookie jar');
  assert_equals(
      await getCookieStringHttp(),
      'HTTP-cookie=value',
      'Cookie we wrote using HTTP in HTTP cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-cookie', value: 'value'}]},
    'Cookie we wrote using HTTP is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-cookie=new-value; path=/');
  assert_equals(
      await getCookieString(),
      'HTTP-cookie=new-value',
      'Cookie we overwrote using HTTP in cookie jar');
  assert_equals(
      await getCookieStringHttp(),
      'HTTP-cookie=new-value',
      'Cookie we overwrote using HTTP in HTTP cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-cookie', value: 'new-value'}]},
    'Cookie we overwrote using HTTP is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-cookie=DELETED; path=/; max-age=0');
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after HTTP cookie-clearing using max-age=0');
  assert_equals(
      await getCookieStringHttp(),
      undefined,
      'Empty HTTP cookie jar after HTTP cookie-clearing using max-age=0');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'HTTP-cookie'}]},
    'Deletion observed after HTTP cookie-clearing using max-age=0');
}, 'HTTP set/overwrite/delete observed in CookieStore');


cookie_test(async t => {
  const eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-cookie=ALREADY-EXPIRED; path=/; max-age=0');
  await setCookieStringHttp('HTTP-alt-cookie=IGNORE; path=/; max-age=10');
  assert_equals(
    await getCookieStringHttp(),
    'HTTP-alt-cookie=IGNORE',
    'Already-expired cookie not included in HTTP');
  await verifyCookieChangeEvent(
    eventPromise,
    {deleted: [], changed: [{name: 'HTTP-alt-cookie', value: 'IGNORE'}]},
    'Deletion not observed after HTTP sets already-expired cookie');
}, 'HTTP set already-expired cookie should not be observed by CookieStore');

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-cookie=VALUE; path=/');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-cookie', value: 'VALUE'}]},
    'Original cookie is observed.');

  eventPromise = observeNextCookieChangeEvent();
  // Overwrite the original cookie with a duplicate, this should not dispatch an event.
  await setCookieStringHttp('HTTP-cookie=VALUE; path=/');
  await setCookieStringHttp('HTTP-alt-cookie=IGNORE; path=/');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-alt-cookie', value: 'IGNORE'}]},
    'Duplicate cookie is not observed.');
}, 'HTTP duplicate cookie should not be observed by CookieStore');


cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-🍪=🔵; path=/');
  assert_equals(
      await getCookieString(),
      'HTTP-🍪=🔵',
      'Cookie we wrote using HTTP in cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-🍪', value: '🔵'}]},
    'Cookie we wrote using HTTP is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTP-🍪=DELETED; path=/; max-age=0');
  assert_equals(
      await getCookieString(),
      undefined,
    'Empty cookie jar after HTTP cookie-clearing using max-age=0');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'HTTP-🍪'}]},
    'Deletion observed after HTTP cookie-clearing using max-age=0');

}, 'CookieStore agreed with HTTP headers agree on encoding non-ASCII cookies');


cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('TEST', 'value0');
  assert_equals(
    await getCookieString(),
    'TEST=value0',
    'Cookie jar contains only cookie we set');
  assert_equals(
    await getCookieStringHttp(),
    'TEST=value0',
    'HTTP cookie jar contains only cookie we set');
  await verifyCookieChangeEvent(
    eventPromise,
    {changed: [{name: 'TEST', value: 'value0'}]},
    'Observed value that was set');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('TEST', 'value');
  assert_equals(
    await getCookieString(),
    'TEST=value',
    'Cookie jar contains only cookie we set');
  assert_equals(
    await getCookieStringHttp(),
    'TEST=value',
    'HTTP cookie jar contains only cookie we set');
  await verifyCookieChangeEvent(
    eventPromise,
    {changed: [{name: 'TEST', value: 'value'}]},
    'Observed value that was overwritten');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.delete('TEST');
  assert_equals(
    await getCookieString(),
    undefined,
    'Cookie jar does not contain cookie we deleted');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'HTTP cookie jar does not contain cookie we deleted');
  await verifyCookieChangeEvent(
    eventPromise,
    {deleted: [{name: 'TEST'}]},
    'Observed cookie that was deleted');
}, 'CookieStore set/overwrite/delete observed in HTTP headers');


cookie_test(async t => {
  await cookieStore.set('🍪', '🔵');
  assert_equals(
    await getCookieStringHttp(),
    '🍪=🔵',
    'HTTP cookie jar contains only cookie we set');

  await cookieStore.delete('🍪');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'HTTP cookie jar does not contain cookie we deleted');
}, 'HTTP headers agreed with CookieStore on encoding non-ASCII cookies');


cookie_test(async t => {
  // Non-UTF-8 byte sequences cause the Set-Cookie to be dropped.
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieBinaryHttp(
      unescape(encodeURIComponent('HTTP-cookie=value')) + '\xef\xbf\xbd; path=/');
  assert_equals(
      await getCookieString(),
      'HTTP-cookie=value\ufffd',
      'Binary cookie we wrote using HTTP in cookie jar');
  assert_equals(
      await getCookieStringHttp(),
      'HTTP-cookie=value\ufffd',
      'Binary cookie we wrote using HTTP in HTTP cookie jar');
  assert_equals(
      decodeURIComponent(escape(await getCookieBinaryHttp())),
      'HTTP-cookie=value\ufffd',
      'Binary cookie we wrote in binary HTTP cookie jar');
  assert_equals(
      await getCookieBinaryHttp(),
      unescape(encodeURIComponent('HTTP-cookie=value')) + '\xef\xbf\xbd',
      'Binary cookie we wrote in binary HTTP cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-cookie', value: 'value\ufffd'}]},
    'Binary cookie we wrote using HTTP is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieBinaryHttp(
      unescape(encodeURIComponent('HTTP-cookie=new-value')) + '\xef\xbf\xbd; path=/');
  assert_equals(
      await getCookieString(),
      'HTTP-cookie=new-value\ufffd',
      'Binary cookie we overwrote using HTTP in cookie jar');
  assert_equals(
      await getCookieStringHttp(),
      'HTTP-cookie=new-value\ufffd',
      'Binary cookie we overwrote using HTTP in HTTP cookie jar');
  assert_equals(
      decodeURIComponent(escape(await getCookieBinaryHttp())),
      'HTTP-cookie=new-value\ufffd',
      'Binary cookie we overwrote in binary HTTP cookie jar');
  assert_equals(
      await getCookieBinaryHttp(),
      unescape(encodeURIComponent('HTTP-cookie=new-value')) + '\xef\xbf\xbd',
      'Binary cookie we overwrote in binary HTTP cookie jar');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'HTTP-cookie', value: 'new-value\ufffd'}]},
    'Binary cookie we overwrote using HTTP is observed');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieBinaryHttp(
      unescape(encodeURIComponent('HTTP-cookie=DELETED; path=/; max-age=0')));
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after binary HTTP cookie-clearing using max-age=0');
  assert_equals(
      await getCookieStringHttp(),
      undefined,
      'Empty HTTP cookie jar after' +
        ' binary HTTP cookie-clearing using max-age=0');
  assert_equals(
      await getCookieBinaryHttp(),
      undefined,
      'Empty binary HTTP cookie jar after' +
        ' binary HTTP cookie-clearing using max-age=0');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: 'HTTP-cookie'}]},
    'Deletion observed after binary HTTP cookie-clearing using max-age=0');
}, 'Binary HTTP set/overwrite/delete observed in CookieStore');
