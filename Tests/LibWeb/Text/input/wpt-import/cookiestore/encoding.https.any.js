// META: title=Cookie Store API: cookie encoding
// META: global=window,serviceworker
// META: script=resources/cookie-test-helpers.js

'use strict';

promise_test(async t => {
  await setCookieStringHttp('\uFEFFcookie=value; path=/');
  t.add_cleanup(async () => {
    await setCookieStringHttp('\uFEFFcookie=value; path=/; Max-Age=0');
  });
  const cookie = await cookieStore.get('\uFEFFcookie');
  assert_equals(cookie.name, '\uFEFFcookie');
  assert_equals(cookie.value, 'value');
}, 'BOM not stripped from name');

promise_test(async t => {
  await setCookieStringHttp('cookie=\uFEFFvalue; path=/');
  t.add_cleanup(async () => {
    await setCookieStringHttp('cookie=\uFEFFvalue; path=/; Max-Age=0');
  });
  const cookie = await cookieStore.get('cookie');
  assert_equals(cookie.name, 'cookie');
  assert_equals(cookie.value, '\uFEFFvalue');
}, 'BOM not stripped from value');
