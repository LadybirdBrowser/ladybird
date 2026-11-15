// META: title=Cookie Store API: Observing 'change' events in document when setting a cookie value containing "="
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('', 'first-value');
  const initialCookies = await cookieStore.getAll('');
  assert_equals(initialCookies.length, 1);
  assert_equals(initialCookies[0].name, '');
  assert_equals(initialCookies[0].value, 'first-value');

  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: '', value: 'first-value'}]},
    'Observed no-name change');

  await promise_rejects_js(
    t,
    TypeError,
    cookieStore.set('', 'suspicious-value=resembles-name-and-value'),
    'Expected promise rejection when setting a cookie with' +
      ' no name and "=" in value (via arguments)');

  await promise_rejects_js(
    t,
    TypeError,
    cookieStore.set(
      {name: '', value: 'suspicious-value=resembles-name-and-value'}),
    'Expected promise rejection when setting a cookie with' +
      ' no name and "=" in value (via options)');

  const cookies = await cookieStore.getAll('');
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, '');
  assert_equals(cookies[0].value, 'first-value',
      'Cookie with no name should still have previous value.');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.delete('');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: ''}]},
    'Observed no-name deletion');

}, "Verify that attempting to set a cookie with no name and with '=' in" +
             " the value does not work.");
