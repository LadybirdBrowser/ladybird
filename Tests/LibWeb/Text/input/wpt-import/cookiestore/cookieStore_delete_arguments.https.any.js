// META: title=Cookie Store API: cookieStore.delete() arguments
// META: script=resources/cookie-test-helpers.js
// META: global=window,serviceworker

'use strict';
const MAX_COOKIE_NAME_SIZE = 4096;

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Max-Age=0`);
  });
  await cookieStore.delete('cookie-name');
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with positional name');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Max-Age=0`);
  });

  await cookieStore.delete({ name: 'cookie-name' });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with name in options');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentDomain = currentUrl.hostname;

  await promise_rejects_js(testCase, TypeError, cookieStore.set(
      { name: 'cookie-name', value: 'cookie-value',
        domain: `.${currentDomain}` }));
}, 'cookieStore.delete domain starts with "."');

promise_test(async testCase => {
  await promise_rejects_js(testCase, TypeError, cookieStore.set(
      { name: 'cookie-name', value: 'cookie-value', domain: 'example.com' }));
}, 'cookieStore.delete with domain that is not equal current host');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentDomain = currentUrl.hostname;
  await cookieStore.set(
      { name: 'cookie-name', value: 'cookie-value', domain: currentDomain });
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Domain=${currentDomain}; Max-Age=0`);
  });

  await cookieStore.delete({ name: 'cookie-name', domain: currentDomain });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with domain set to the current hostname');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentDomain = currentUrl.hostname;
  const subDomain = `sub.${currentDomain}`;

  await promise_rejects_js(testCase, TypeError, cookieStore.delete(
      { name: 'cookie-name', domain: subDomain }));
}, 'cookieStore.delete with domain set to a subdomain of the current hostname');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentDomain = currentUrl.hostname;
  assert_not_equals(currentDomain[0] === '.',
      'this test assumes that the current hostname does not start with .');
  const domainSuffix = currentDomain.substr(1);

  await promise_rejects_js(testCase, TypeError, cookieStore.delete(
      { name: 'cookie-name', domain: domainSuffix }));
}, 'cookieStore.delete with domain set to a non-domain-matching suffix of ' +
   'the current hostname');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentPath = currentUrl.pathname;
  const currentDirectory =
      currentPath.substr(0, currentPath.lastIndexOf('/') + 1);
  await cookieStore.set(
      { name: 'cookie-name', value: 'cookie-value', path: currentDirectory });
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}; Max-Age=0`);
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}/; Max-Age=0`);
  });

  await cookieStore.delete({ name: 'cookie-name', path: currentDirectory });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with path set to the current directory');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentPath = currentUrl.pathname;
  const currentDirectory =
      currentPath.substr(0, currentPath.lastIndexOf('/') + 1);
  const subDirectory = currentDirectory + "subdir/";
  await cookieStore.set(
      { name: 'cookie-name', value: 'cookie-value', path: currentDirectory });
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}; Max-Age=0`);
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}/; Max-Age=0`);
  });

  await cookieStore.delete({ name: 'cookie-name', path: subDirectory });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie.name, 'cookie-name');
  assert_equals(cookie.value, 'cookie-value');
}, 'cookieStore.delete with path set to subdirectory of the current directory');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentPath = currentUrl.pathname;
  const currentDirectory = currentPath.substr(0, currentPath.lastIndexOf('/'));
  await setCookieStringHttp(`cookie-name=cookie-value; Path=${currentDirectory};`);

  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}; Max-Age=0`);
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}/; Max-Age=0`);
  });

  await cookieStore.delete({ name: 'cookie-name', path: currentDirectory });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete does not append / at the end of path');

promise_test(async testCase => {
  if (typeof self.document === 'undefined') {
    // The test is being run from a service worker context where document is undefined
    testCase.done();
    return;
  }
  const currentUrl = new URL(self.location.href);
  const currentPath = currentUrl.pathname;
  const currentDirectory = currentPath.substr(0, currentPath.lastIndexOf('/'));
  await setCookieStringDocument('cookie-name=cookie-value; path=' + currentDirectory);
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}; Max-Age=0`);
    await setCookieStringHttp(`cookie-name=deleted; Path=${currentDirectory}/; Max-Age=0`);
  });
  await cookieStore.delete({ name: 'cookie-name', path: currentDirectory });
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete can delete a cookie set by document.cookie if document is defined');

promise_test(async testCase => {
  const currentUrl = new URL(self.location.href);
  const currentPath = currentUrl.pathname;
  const currentDirectory =
      currentPath.substr(0, currentPath.lastIndexOf('/') + 1);
  const invalidPath = currentDirectory.substr(1);

  await promise_rejects_js(testCase, TypeError, cookieStore.delete(
      { name: 'cookie-name', path: invalidPath }));
}, 'cookieStore.delete with path that does not start with /');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`cookie-name=deleted; Max-Age=0`);
  });

  const cookie_attributes = await cookieStore.get('cookie-name');
  assert_equals(cookie_attributes.name, 'cookie-name');
  assert_equals(cookie_attributes.value, 'cookie-value');

  await cookieStore.delete(cookie_attributes.name);
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with get result');

promise_test(async testCase => {
  await cookieStore.set('', 'cookie-value');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`=deleted; Max-Age=0`);
  });

  await cookieStore.delete('');
  const cookie = await cookieStore.get('');
  assert_equals(cookie, null);
}, 'cookieStore.delete with positional empty name');

promise_test(async testCase => {
  await cookieStore.set('', 'cookie-value');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(`=deleted; Max-Age=0`);
  });

  await cookieStore.delete({ name: '' });
  const cookie = await cookieStore.get('');
  assert_equals(cookie, null);
}, 'cookieStore.delete with empty name in options');

promise_test(async testCase => {
  const cookieName = 't'.repeat(MAX_COOKIE_NAME_SIZE);
  await cookieStore.set(cookieName, '');
  testCase.add_cleanup(async () => {
    await setCookieStringHttp(cookieName + '=; Max-Age=0');
  });

  await cookieStore.delete({name: cookieName});
  const cookie = await cookieStore.get(cookieName);
  assert_equals(cookie, null);
}, 'cookieStore.delete with maximum cookie name size');

promise_test(async testCase => {
  // Cookies having a __Host- prefix are not allowed to specify a domain
  await cookieStore.delete('cookie-name');

  const currentUrl = new URL(self.location.href);
  const currentDomain = currentUrl.hostname;

  await promise_rejects_js(testCase, TypeError, cookieStore.delete(
      { name: '__Host-cookie-name',
        value: 'cookie-value',
        domain: currentDomain }));
  const cookie = await cookieStore.get('cookie-name');
  assert_equals(cookie, null);
}, 'cookieStore.delete with a __Host- prefix should not have a domain');

promise_test(async testCase => {
  await cookieStore.set('cookie-name', 'cookie-value');
  testCase.add_cleanup(async () => {
    await cookieStore.delete('cookie-name');
  });
  const cookie = await cookieStore.delete(' cookie-name \t');
  assert_equals(cookie, undefined);
}, 'cookieStore.delete with whitespace');
