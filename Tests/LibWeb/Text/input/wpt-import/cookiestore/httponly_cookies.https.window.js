// META: script=resources/cookie-test-helpers.js

'use strict';

promise_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp('HTTPONLY-cookie=value; path=/; httponly');
  t.add_cleanup(async () => {
    await setCookieStringHttp(`HTTPONLY-cookie=DELETED; path=/; httponly; Max-Age=0`);
  });
  assert_equals(
      await getCookieString(),
      undefined,
      'HttpOnly cookie we wrote using HTTP in cookie jar' +
        ' is invisible to script');
  assert_equals(
      await getCookieStringHttp(),
      'HTTPONLY-cookie=value',
    'HttpOnly cookie we wrote using HTTP in HTTP cookie jar');

  await setCookieStringHttp('HTTPONLY-cookie=new-value; path=/; httponly');
  assert_equals(
      await getCookieString(),
      undefined,
      'HttpOnly cookie we overwrote using HTTP in cookie jar' +
        ' is invisible to script');
  assert_equals(
      await getCookieStringHttp(),
      'HTTPONLY-cookie=new-value',
    'HttpOnly cookie we overwrote using HTTP in HTTP cookie jar');

  eventPromise = observeNextCookieChangeEvent();
  await setCookieStringHttp(
      'HTTPONLY-cookie=DELETED; path=/; max-age=0; httponly');
  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after HTTP cookie-clearing using max-age=0');
  assert_equals(
      await getCookieStringHttp(),
      undefined,
      'Empty HTTP cookie jar after HTTP cookie-clearing using max-age=0');

  // HTTPONLY cookie changes should not have been observed; perform
  // a dummy change to verify that nothing else was queued up.
  await cookieStore.set('TEST', 'dummy');
  t.add_cleanup(async () => {
    await cookieStore.delete('TEST');
  });
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'TEST', value: 'dummy'}]},
    'HttpOnly cookie deletion was not observed');
}, 'HttpOnly cookies are not observed');


cookie_test(async t => {
  document.cookie = 'cookie1=value1; path=/';
  document.cookie = 'cookie2=value2; path=/; httponly';
  document.cookie = 'cookie3=value3; path=/';
  assert_equals(
    await getCookieStringHttp(), 'cookie1=value1; cookie3=value3',
    'Trying to store an HttpOnly cookie with document.cookie fails');
}, 'HttpOnly cookies can not be set by document.cookie');


// Historical: Early iterations of the proposal included an httpOnly option.
cookie_test(async t => {
  await cookieStore.set('cookie1', 'value1');
  await cookieStore.set('cookie2', 'value2', {httpOnly: true});
  await cookieStore.set('cookie3', 'value3');
  assert_equals(
    await getCookieStringHttp(),
    'cookie1=value1; cookie2=value2; cookie3=value3',
    'httpOnly is not an option for CookieStore.set()');
}, 'HttpOnly cookies can not be set by CookieStore');

promise_test(async t => {
  await setCookieStringHttp('HTTPONLY-cookie=value; path=/; httponly');
  t.add_cleanup(async () => {
    await setCookieStringHttp(`HTTPONLY-cookie=DELETED; path=/; httponly; Max-Age=0`);
  });
  assert_equals(
      await getCookieString(),
      undefined,
      'HttpOnly cookie we wrote using HTTP in cookie jar' +
        ' is invisible to script');
  assert_equals(
      await getCookieStringHttp(),
      'HTTPONLY-cookie=value',
    'HttpOnly cookie we wrote using HTTP in HTTP cookie jar');

  try {
    await cookieStore.set('HTTPONLY-cookie', 'dummy');
  } catch(e) {}

  assert_equals(
      await getCookieString(),
      undefined,
      'HttpOnly cookie is not overwritten');

  try {
    await cookieStore.delete('HTTPONLY-cookie');
  } catch(e) {}

  assert_equals(await getCookieString(), undefined, 'HttpOnly cookie is not overwritten');

  assert_equals(await getCookieStringHttp(), 'HTTPONLY-cookie=value', 'HttpOnly cookie is not deleted');
}, 'HttpOnly cookies are not deleted/overwritten');
