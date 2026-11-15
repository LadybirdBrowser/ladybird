// META: title=Cookie Store API: Observing 'change' events in document when modifications API is called multiple times with a blank name
// META: script=resources/cookie-test-helpers.js

'use strict';

cookie_test(async t => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('', 'first-value');
  let actual1 =
      (await cookieStore.getAll('')).map(({ value }) => value).join(';');
  let expected1 = 'first-value';
  assert_equals(actual1, expected1);
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: '', value: 'first-value'}]},
    'Observed no-name change');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set('', 'second-value');
  let actual2 =
      (await cookieStore.getAll('')).map(({ value }) => value).join(';');
  let expected2 = 'second-value';
  assert_equals(actual2, expected2);
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: '', value: 'second-value'}]},
    'Observed no-name change');

  eventPromise = observeNextCookieChangeEvent();
  await cookieStore.delete('');
  await verifyCookieChangeEvent(
    eventPromise, {deleted: [{name: ''}]},
    'Observed no-name change');

  assert_equals(
      await getCookieString(),
      undefined,
      'Empty cookie jar after testNoNameMultipleValues');
  assert_equals(
    await getCookieStringHttp(),
    undefined,
    'Empty HTTP cookie jar after testNoNameMultipleValues');
}, 'Verify behavior of multiple no-name cookies');
