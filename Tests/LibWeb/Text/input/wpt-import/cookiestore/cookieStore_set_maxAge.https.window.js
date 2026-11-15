// META: title=Cookie Store API: cookieStore.set() maxAge
// META: script=resources/cookie-test-helpers.js
// META: global=window,serviceworker
cookie_test(async testCase => {
  await cookieStore.set(
    {
      name: 'cookie-name',
      value: 'cookie-value',
      maxAge: 60
    });

  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.set with maxAge set to a positive value');

cookie_test(async testCase => {
  await cookieStore.set(
    {
      name: 'cookie-name',
      value: 'cookie-value',
      maxAge: -60
    });

  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.set with maxAge set to a negative value');

cookie_test(async testCase => {
  let eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set(
    {
      name: 'cookie-name',
      value: 'cookie-value',
      maxAge: 0
    });

  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
  await cookieStore.set('alt-cookie', 'IGNORE');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'alt-cookie', value: 'IGNORE'}]},
    'Already expired cookie is not observed.');
}, 'cookieStore.set with maxAge set to zero, cookie change event not dispatched');

cookie_test(async testCase => {
  const oneDay = 24 * 60 * 60 * 1000;
  const tomorrow = Date.now() + oneDay ;

  await promise_rejects_js(testCase, TypeError,
    cookieStore.set({
      name: 'cookie-name',
      value: 'cookie-value',
      expires: tomorrow,
      maxAge: 60
    }));
}, 'cookieStore.set fails with both maxAge and expires');
