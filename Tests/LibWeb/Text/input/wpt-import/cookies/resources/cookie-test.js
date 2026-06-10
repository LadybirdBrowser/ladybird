// getAndExpireCookiesForDefaultPathTest is a helper method to get and delete
// cookies using echo-cookie.html.
async function getAndExpireCookiesForDefaultPathTest() {
  return new Promise((resolve, reject) => {
    try {
      const iframe = document.createElement('iframe');
      iframe.style = 'display: none';
      iframe.addEventListener('load', (e) => {
        const win = e.target.contentWindow;
        const iframeCookies = win.getCookies();
        win.expireCookies().then(() => {
          document.documentElement.removeChild(iframe);
          resolve(iframeCookies);
        });
      }, {once: true});
      iframe.src = '/cookies/resources/echo-cookie.html';
      document.documentElement.appendChild(iframe);
    } catch (e) {
      reject(e);
    }
  });
}

// getAndExpireCookiesForRedirectTest is a helper method to get and delete
// cookies that were set from a Location header redirect.
async function getAndExpireCookiesForRedirectTest(location) {
  return new Promise((resolve, reject) => {
    try {
      const iframe = document.createElement('iframe');
      iframe.style = 'display: none';
      const listener = (e) => {
        if (typeof e.data == 'object' && 'cookies' in e.data) {
          window.removeEventListener('message', listener);
          document.documentElement.removeChild(iframe);
          resolve(e.data.cookies);
        }
      };
      window.addEventListener('message', listener);
      iframe.addEventListener('load', (e) => {
        e.target.contentWindow.postMessage('getAndExpireCookiesForRedirectTest', '*');
      }, {once: true});
      iframe.src = location;
      document.documentElement.appendChild(iframe);
    } catch (e) {
      reject(e);
    }
  });
}

// httpCookieTest sets a `cookie` (via HTTP), then asserts it was or was not set
// via `expectedValue` (via the DOM). Then cleans it up (via test driver). Most
// tests do not set a Path attribute, so `defaultPath` defaults to true. If the
// cookie values are expected to cause the HTTP request or response to fail, the
// test can be made to pass when this happens via `allowFetchFailure`, which
// defaults to false.
//
// `cookie` may be a single cookie string, or an array of cookie strings, where
// the order of the array items represents the order of the Set-Cookie headers
// sent by the server.
//
// Note: this function has a dependency on testdriver.js. Any test files calling
// it should include testdriver.js and testdriver-vendor.js
function httpCookieTest(cookie, expectedValue, name, defaultPath = true,
                        allowFetchFailure = false) {
  return promise_test((t) => {
    var skipAssertions = false;
    return new Promise(async (resolve, reject) => {
      // The result is ignored as we're expiring cookies for cleaning here.
      await getAndExpireCookiesForDefaultPathTest();
      await test_driver.delete_all_cookies();
      t.add_cleanup(test_driver.delete_all_cookies);

      let encodedCookie = encodeURIComponent(JSON.stringify(cookie));
      try {
        await fetch(`/cookies/resources/cookie.py?set=${encodedCookie}`);
      } catch {
        if (allowFetchFailure) {
          skipAssertions = true;
        } else {
          reject('Failed to fetch /cookies/resources/cookie.py');
        }
      }
      let cookies = document.cookie;
      if (defaultPath) {
        // for the tests where a Path is set from the request-uri
        // path, we need to go look for cookies in an iframe at that
        // default path.
        cookies = await getAndExpireCookiesForDefaultPathTest();
      }
      resolve(cookies);
    }).then((cookies) => {
      if (skipAssertions) {
        return;
      }
      if (Boolean(expectedValue)) {
        assert_equals(cookies, expectedValue, 'The cookie was set as expected.');
      } else {
        assert_equals(cookies, expectedValue, 'The cookie was rejected.');
      }
    });
  }, name);
}

// This is a variation on httpCookieTest, where a redirect happens via
// the Location header and we check to see if cookies are sent via
// getRedirectedCookies
//
// Note: the locations targeted by this function have a dependency on
// path-redirect-shared.js and should be sure to include it.
function httpRedirectCookieTest(cookie, expectedValue, name, location) {
  return promise_test(async (t) => {
    // The result is ignored as we're expiring cookies for cleaning here.
    await getAndExpireCookiesForRedirectTest(location);

    const encodedCookie = encodeURIComponent(JSON.stringify(cookie));
    const encodedLocation = encodeURIComponent(location);
    const setParams = `?set=${encodedCookie}&location=${encodedLocation}`;
    await fetch(`/cookies/resources/cookie.py${setParams}`);
    // for the tests where a redirect happens, we need to head
    // to that URI to get the cookies (and then delete them there)
    const cookies = await getAndExpireCookiesForRedirectTest(location);
    if (Boolean(expectedValue)) {
      assert_equals(cookies, expectedValue, 'The cookie was set as expected.');
    } else {
      assert_equals(cookies, expectedValue, 'The cookie was rejected.');
    }
  }, name);
}

// Sets a `cookie` via the DOM, checks it against `expectedValue` via the DOM,
// then cleans it up via the DOM. This is needed in cases where going through
// HTTP headers may modify the cookie line (e.g. by stripping control
// characters).
//
// Note: this function has a dependency on testdriver.js. Any test files calling
// it should include testdriver.js and testdriver-vendor.js
function domCookieTest(cookie, expectedValue, name) {
  return promise_test(async (t) => {
    await test_driver.delete_all_cookies();
    t.add_cleanup(test_driver.delete_all_cookies);

    if (typeof cookie === "string") {
      document.cookie = cookie;
    } else if (Array.isArray(cookie)) {
      for (const singlecookie of cookie) {
        document.cookie = singlecookie;
      }
    } else {
      throw new Error('Unexpected type passed into domCookieTest as cookie: ' + typeof cookie);
    }
    let cookies = document.cookie;
    assert_equals(cookies, expectedValue, Boolean(expectedValue) ?
                                          'The cookie was set as expected.' :
                                          'The cookie was rejected.');
  }, name);
}

// Returns an array of control characters along with their ASCII codes. Control
// characters are defined by RFC 5234 to be %x00-1F / %x7F.
function getCtlCharacters() {
  const ctlCodes = [...Array(0x20).keys()]
                       .concat([0x7F]);
  return ctlCodes.map(i => ({ code: i, chr: String.fromCharCode(i) }))
}

// Returns a cookie string with name set to "t" * nameLength and value
// set to "1" * valueLength. Passing in 0 for either allows for creating
// a name- or value-less cookie.
//
// Note: Cookie length checking should ignore the "=".
function cookieStringWithNameAndValueLengths(nameLength, valueLength) {
  return `${"t".repeat(nameLength)}=${"1".repeat(valueLength)}`;
}

// Finds the root window.top.opener and directs test_driver commands to it.
//
// If you see a message like: "Error: Tried to run in a non-testharness window
// without a call to set_test_context." then you probably need to call this.
function setTestContextUsingRootWindow() {
  let test_window = window.top;
  while (test_window.opener && !test_window.opener.closed) {
    test_window = test_window.opener.top;
  }
  test_driver.set_test_context(test_window);
}
