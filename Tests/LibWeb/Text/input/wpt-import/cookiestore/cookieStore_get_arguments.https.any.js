// META: title=Cookie Store API: cookieStore.get() arguments
// META: global=window,serviceworker

'use strict';

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  await promise_rejects_js(testCase, TypeError, cookieStore.get());
}, 'cookieStore.get with no arguments returns TypeError');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  await promise_rejects_js(testCase, TypeError, cookieStore.get({}));
},'cookieStore.get with empty options returns TypeError');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with positional name');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const cookie = await cookieStore.get({ name: 'cookie-name' });
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with name in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const cookie = await cookieStore.get('cookie-name',
                                       { name: 'wrong-cookie-name' });
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with name in both positional arguments and options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  let target_url = self.location.href;
  if (self.GLOBAL.isWorker()) {
    target_url = target_url + '/path/within/scope';
  }

  const cookie = await cookieStore.get({ url: target_url });
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with absolute url in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  let target_path = self.location.pathname;
  if (self.GLOBAL.isWorker()) {
    target_path = target_path + '/path/within/scope';
  }

  const cookie = await cookieStore.get({ url: target_path });
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with relative url in options');

if (!self.GLOBAL.isWorker()) {
  promise_test(async testCase => {
    const invalid_url =
        `${self.location.protocol}//${self.location.host}/different/path`;
    await promise_rejects_js(testCase, TypeError, cookieStore.get(
        { url: invalid_url }));
  }, 'cookieStore.get with invalid url path in options');
} else {
  promise_test(async testCase => {
    const sameorigin_url =
        `${self.location.protocol}//${self.location.host}/different/path`;
    assert_true(await cookieStore.get({ url: sameorigin_url }) === null);
  }, 'cookieStore.get with same-origin url path in options');
}

promise_test(async testCase => {
  const invalid_url =
      `${self.location.protocol}//www.example.com${self.location.pathname}`;
  await promise_rejects_js(testCase, TypeError, cookieStore.get(
      { url: invalid_url }));
}, 'cookieStore.get with invalid url host in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  let target_url = self.location.href;
  if (self.GLOBAL.isWorker()) {
    target_url = target_url + '/path/within/scope';
  }

  target_url = target_url + "#foo";

  const cookie = await cookieStore.get({ url: target_url });
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with absolute url with fragment in options');

promise_test(async testCase => {
  if (!self.GLOBAL.isWorker()) {
    await cookieStore.set('cookie-name', 'cookie-value');
    testCase.add_cleanup(async () => {
      await cookieStore.delete('cookie-name');
    });

    self.location = "#foo";
    let target_url = self.location.href;

    const cookie = await cookieStore.get({ url: target_url });
    assert_equals(cookie.name, 'cookie-name');
    assert_equals(cookie.value, 'cookie-value');
  }
}, 'cookieStore.get with absolute different url in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  const cookie = await cookieStore.get(' cookie-name \t');
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.get with whitespace');
