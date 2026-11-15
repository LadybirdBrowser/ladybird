// META: title=Cookie Store API: Observing 'change' events in document when modifications API is called with blank arguments
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('', 'first-value');
  const actual1 =
      (await cookieStore.getAll('')).map(({ value }) => value).join(';');
  const expected1 = 'first-value';
  assert_equals(actual1, expected1);
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: '', value: 'first-value'}]},
    'Observed no-name change');

  await promise_rejects_js(
    t,
    TypeError,
    cookieStore.set('', ''),
    'Expected promise rejection when setting a cookie with' +
      ' no name and no value');

  await promise_rejects_js(
    t,
    TypeError,
    cookieStore.set({name: '', value: ''}),
    'Expected promise rejection when setting a cookie with' +
      ' no name and no value');

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

  assert_equals(
    await getCookieString(),
      undefined,
      'Empty cookie jar');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'Empty HTTP cookie jar');
  if (kHasDocument) {
    assert_equals(
      await getCookieStringDocument(),
      undefined,
      'Empty document.cookie cookie jar');
  }

}, 'Verify behavior of no-name and no-value cookies.');
