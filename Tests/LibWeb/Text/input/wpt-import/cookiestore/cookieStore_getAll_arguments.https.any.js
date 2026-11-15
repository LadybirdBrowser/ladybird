// META: title=Cookie Store API: cookieStore.getAll() arguments
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

  const cookies = await cookieStore.getAll();
  cookies.sort((a, b) => a.name.localeCompare(b.name));
  assert_equals(cookies.length, 2);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
  assert_equals(cookies[1].name, 'cookie-name-2');
  assert_equals(cookies[1].value, 'cookie-value-2');
}, 'cookieStore.getAll with no arguments');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  await cookieStore.set('cookie-name-2', 'cookie-value-2');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-2');
  });

  const cookies = await cookieStore.getAll({});
  cookies.sort((a, b) => a.name.localeCompare(b.name));
  assert_equals(cookies.length, 2);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
  assert_equals(cookies[1].name, 'cookie-name-2');
  assert_equals(cookies[1].value, 'cookie-value-2');
}, 'cookieStore.getAll with empty options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  await cookieStore.set('cookie-name-2', 'cookie-value-2');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-2');
  });

  const cookies = await cookieStore.getAll('cookie-name');
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with positional name');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  await cookieStore.set('cookie-name-2', 'cookie-value-2');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-2');
  });

  const cookies = await cookieStore.getAll({ name: 'cookie-name' });
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with name in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  await cookieStore.set('cookie-name-2', 'cookie-value-2');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name-2');
  });

  const cookies = await cookieStore.getAll('cookie-name',
                                           { name: 'wrong-cookie-name' });
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with name in both positional arguments and options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  let target_url = self.location.href;
  if (self.GLOBAL.isWorker()) {
    target_url = target_url + '/path/within/scope';
  }

  const cookies = await cookieStore.getAll({ url: target_url });
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with absolute url in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  let target_path = self.location.pathname;
  if (self.GLOBAL.isWorker()) {
    target_path = target_path + '/path/within/scope';
  }

  const cookies = await cookieStore.getAll({ url: target_path });
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with relative url in options');

if (!self.GLOBAL.isWorker()) {
  promise_test(async testCase => {
    const invalid_url =
        `${self.location.protocol}//${self.location.host}/different/path`;
    await promise_rejects_js(testCase, TypeError, cookieStore.getAll(
        { url: invalid_url }));
  }, 'cookieStore.getAll with invalid url path in options');
} else {
  promise_test(async testCase => {
    await cookieStore.set('cookie-name', 'cookie-value');
    testCase.add_cleanup(async () => {
      await cookieStore.delete('cookie-name');
    });

    const sameorigin_url =
        `${self.location.protocol}//${self.location.host}/different/path`;
    const cookies = await cookieStore.getAll({ url: sameorigin_url });
    assert_equals(cookies.length, 1);
    assert_equals(cookies[0].name, 'cookie-name');
    assert_equals(cookies[0].value, 'cookie-value');
  }, 'cookieStore.getAll with same-origin url path in options');
}

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });

  const invalid_url =
      `${self.location.protocol}//www.example.com${self.location.pathname}`;
  await promise_rejects_js(testCase, TypeError, cookieStore.getAll(
      { url: invalid_url }));
}, 'cookieStore.getAll with invalid url host in options');

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

  const cookies = await cookieStore.getAll({ url: target_url });
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with absolute url with fragment in options');

promise_test(async testCase => {
  if (!self.GLOBAL.isWorker()) {
    await cookieStore.set('cookie-name', 'cookie-value');
    testCase.add_cleanup(async () => {
      await cookieStore.delete('cookie-name');
    });

    self.location = "#foo";
    let target_url = self.location.href;

    const cookies = await cookieStore.getAll({ url: target_url });
    assert_equals(cookies.length, 1);
    assert_equals(cookies[0].name, 'cookie-name');
    assert_equals(cookies[0].value, 'cookie-value');
  }
}, 'cookieStore.getAll with absolute different url in options');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  const cookies = await cookieStore.getAll(' cookie-name \t');
  assert_equals(cookies.length, 1);
  assert_equals(cookies[0].name, 'cookie-name');
  assert_equals(cookies[0].value, 'cookie-value');
}, 'cookieStore.getAll with whitespace');
