'use strict';

promise_test(async testCase => {
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  // The events are asynchronously dispatched. Let's wait for both in the
  // expected order to avoid race conditions.

  {
    const eventPromise = new Promise((resolve) => {
      cookieStore.onchange = resolve;
    });
    await cookieStore.set('cookie-name', 'cookie-value');

    const event = await eventPromise;
    assert_true(event instanceof CookieChangeEvent);
    assert_equals(event.type, 'change');
    assert_equals(event.changed.length, 1);
    assert_equals(event.changed[0].name, 'cookie-name');
  }

  {
    const eventPromise = new Promise((resolve) => {
      cookieStore.onchange = resolve;
    });
    await cookieStore.delete('cookie-name');
    const event = await eventPromise;
    assert_true(event instanceof CookieChangeEvent);
    assert_equals(event.type, 'change');
    assert_equals(event.deleted.length, 1);
    assert_equals(event.deleted[0].name, 'cookie-name');
    assert_equals(
        event.deleted[0].value, undefined,
        'Cookie change events for deletions should not have cookie values');
    assert_equals(event.changed.length, 0);
  }
}, 'cookieStore fires change event for cookie deleted by cookieStore.delete()');

promise_test(async testCase => {
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const eventPromise = new Promise((resolve) => {
    const events = [];
    cookieStore.onchange = event => {
      events.push(event);
      if (event.type === 'change' &&
          event.deleted.length === 1 &&
          event.deleted[0].name === 'cookie-name') {
         resolve(events);
       }
    }
  });

  await cookieStore.delete('cookie-unknown');
  await cookieStore.set('cookie-name', 'cookie-value');
  await cookieStore.delete('cookie-another-unknown');
  await cookieStore.delete('cookie-name');

  const events = await eventPromise;

  assert_equals(events.length, 2);
  assert_true(events[0] instanceof CookieChangeEvent);
  assert_equals(events[0].type, 'change');
  assert_equals(events[0].changed.length, 1);
  assert_equals(events[0].changed[0].name, 'cookie-name');

  assert_true(events[1] instanceof CookieChangeEvent);
  assert_equals(events[1].type, 'change');
  assert_equals(events[1].deleted.length, 1);
  assert_equals(events[1].deleted[0].name, 'cookie-name');
}, 'cookieStore does not fire change events for non-existing expired cookies');
