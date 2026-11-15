'use strict';

promise_test(async testCase => {
  const eventPromise = new Promise((resolve) => {
    cookieStore.onchange = resolve;
  });

  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const event = await eventPromise;
  assert_true(event instanceof CookieChangeEvent);

  assert_equals(event.changed, event.changed);
  assert_equals(event.deleted, event.deleted);

  assert_equals(event.type, 'change');
  assert_equals(event.changed.length, 1);
  assert_equals(event.changed[0].name, 'cookie-name');
  assert_equals(event.changed[0].value, 'cookie-value');
  assert_equals(event.deleted.length, 0);
}, 'cookieStore fires change event for cookie set by cookieStore.set()');
