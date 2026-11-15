// META: title=Cookie Store API: cookieStore.set()/get()/delete() for cookies with special names
// META: global=window,serviceworker

'use strict';

['__Secure-', '__secure-', '__Host-', '__host-'].forEach(prefix => {
  promise_test(async testCase => {
    await cookieStore.set(`${prefix}cookie-name`, `secure-cookie-value`);
    assert_equals(
      (await cookieStore.get(`${prefix}cookie-name`)).value,
      'secure-cookie-value',
      `Setting ${prefix} cookies should not fail in secure context`);

    try { await cookieStore.delete(`${prefix}cookie-name`); } catch (e) {}
  }, `cookieStore.set with ${prefix} name on secure origin`);

  promise_test(async testCase => {
    // This test is for symmetry with the non-secure case. In non-secure
    // contexts, the set() should fail even if the expiration date makes
    // the operation a no-op.
    await cookieStore.set(
        { name: `${prefix}cookie-name`, value: `secure-cookie-value`,
          expires: Date.now() - (24 * 60 * 60 * 1000)});
    assert_equals(await cookieStore.get(`${prefix}cookie-name`), null);
    try { await cookieStore.delete(`${prefix}cookie-name`); } catch (e) {}
  }, `cookieStore.set of expired ${prefix} cookie name on secure origin`);

  promise_test(async testCase => {
    assert_equals(
      await cookieStore.delete(`${prefix}cookie-name`), undefined,
      `Deleting ${prefix} cookies should not fail in secure context`);
  }, `cookieStore.delete with ${prefix} name on secure origin`);
});

['__Host-', '__host-'].forEach(prefix => {
  promise_test(async testCase => {
    const currentUrl = new URL(self.location.href);
    const currentDomain = currentUrl.hostname;
    await promise_rejects_js(testCase, TypeError,
        cookieStore.set({ name: `${prefix}cookie-name`, value: 'cookie-value',
                          domain: currentDomain }));
  }, `cookieStore.set with ${prefix} prefix and a domain option`);

  promise_test(async testCase => {
    await cookieStore.set({ name: `${prefix}cookie-name`, value: 'cookie-value',
                            path: "/" });

    assert_equals(
        (await cookieStore.get(`${prefix}cookie-name`)).value, "cookie-value");

    await promise_rejects_js(testCase, TypeError,
        cookieStore.set( { name: `${prefix}cookie-name`, value: 'cookie-value',
                          path: "/path" }));

    await cookieStore.delete({ name: `${prefix}cookie-name`});
  }, `cookieStore.set with ${prefix} prefix a path option`);
});

['__Host-Http-', '__host-http-', '__Http-', '__http-', '  __Http-', '\t__Http-',
 '  __Host-Http-', '\t__Host-Http-'].forEach(prefix => {
  promise_test(async testCase => {
    await promise_rejects_js(testCase, TypeError,
        cookieStore.set({ name: `${prefix}cookie-name`, value: 'cookie-value'}));
  }, `cookieStore.set with ${prefix} prefix rejects`);
});

promise_test(async testCase => {
    let exceptionThrown = false;
    try {
        await cookieStore.set(unescape('cookie-name%0D1'), 'cookie-value');
    } catch (e) {
        assert_equals (e.name, "TypeError", "cookieStore thrown an incorrect exception -");
        exceptionThrown = true;
    }
    assert_true(exceptionThrown, "No exception thrown.");
}, 'cookieStore.set with malformed name.');

['__Host-', '__Secure-', '__Http-', '__Host-Http-', ' __Host-', '\t__Host-', ' __Secure-',
 '\t__Secure-', ' __Http-', '\t__Http-', ' __Host-Http-', '\t__Host-Http-'].forEach(prefix => {
  promise_test(async testCase => {
    // Nameless cookies cannot have special prefixes
    await cookieStore.delete('');

    const currentUrl = new URL(self.location.href);
    const currentDomain = currentUrl.hostname;

    await promise_rejects_js(testCase, TypeError, cookieStore.set(
        { name: '',
          value: `${prefix}nameless-cookie`,
          domain: `.${currentDomain}` }));
    const cookie = await cookieStore.get('');
    assert_equals(cookie, null);
  }, `cookieStore.set a nameless cookie cannot have ${prefix} prefix`);
});
