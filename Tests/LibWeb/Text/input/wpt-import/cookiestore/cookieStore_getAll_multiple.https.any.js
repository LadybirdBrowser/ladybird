// META: title=Cookie Store API: cookieStore.getAll() with multiple cookies
// META: global=window,serviceworker

'use strict';

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  await cookieStore.set('cookie-name-2', 'cookie-value-2');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-2');
  });
  await cookieStore.set('cookie-name-3', 'cookie-value-3');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-3');
  });

  const cookies = await cookieStore.getAll();
  cookies.sort((a, b) => a.name.localeCompare(b.name));
  assert_equals(cookies.length, 3);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
  assert_equals(cookies[1].name, 'cookie-name-2');
  assert_equals(cookies[1].value, 'cookie-value-2');
  assert_equals(cookies[2].name, 'cookie-name-3');
  assert_equals(cookies[2].value, 'cookie-value-3');
}, 'cookieStore.getAll returns multiple cookies written by cookieStore.set');
