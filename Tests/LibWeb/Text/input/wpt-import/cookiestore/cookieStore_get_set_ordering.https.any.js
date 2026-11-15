// META: title=Cookie Store API: Cookie ordering
// META: global=window,serviceworker

'use strict';

promise_test(async t => {
  t.add_cleanup(async () => {
    await cookieStore.delete('ordered-1');
    await cookieStore.delete('ordered-2');
    await cookieStore.delete('ordered-3');
  });

  await cookieStore.set('ordered-1', 'cookie-value1');
  await cookieStore.set('ordered-2', 'cookie-value2');
  await cookieStore.set('ordered-3', 'cookie-value3');
  // NOTE: this assumes no concurrent writes from elsewhere; it also
  // uses three separate cookie jar read operations where a single getAll
  // would be more efficient, but this way the CookieStore does the filtering
  // for us.
  const matchingValues = await Promise.all(['1', '2', '3'].map(
      async suffix => (await cookieStore.get('ordered-' + suffix)).value));
  const actual = matchingValues.join(';');
  const expected = 'cookie-value1;cookie-value2;cookie-value3';
  assert_equals(actual, expected);
}, 'Set three simple origin session cookies sequentially and ensure ' +
            'they all end up in the cookie jar in order.');

promise_test(async t => {
  t.add_cleanup(async () => {
    await cookieStore.delete('ordered-unordered1');
    await cookieStore.delete('ordered-unordered2');
    await cookieStore.delete('ordered-unordered3');
  });

  await Promise.all([
    cookieStore.set('ordered-unordered1', 'unordered-cookie-value1'),
    cookieStore.set('ordered-unordered2', 'unordered-cookie-value2'),
    cookieStore.set('ordered-unordered3', 'unordered-cookie-value3')
  ]);
  // NOTE: this assumes no concurrent writes from elsewhere; it also
  // uses three separate cookie jar read operations where a single getAll
  // would be more efficient, but this way the CookieStore does the filtering
  // for us and we do not need to sort.
  const matchingCookies = await Promise.all(['1', '2', '3'].map(
    suffix => cookieStore.get('ordered-unordered' + suffix)));
  const actual = matchingCookies.map(({ value }) => value).join(';');
  const expected =
      'unordered-cookie-value1;' +
      'unordered-cookie-value2;' +
      'unordered-cookie-value3';
  assert_equals(actual, expected);
}, 'Set three simple origin session cookies in undefined order using ' +
            'Promise.all and ensure they all end up in the cookie jar in any ' +
            'order. ');
