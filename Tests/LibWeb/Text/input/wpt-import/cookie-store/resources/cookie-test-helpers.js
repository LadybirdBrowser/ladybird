'use strict';

// TODO(jsbell): Once ServiceWorker is supported, add arbitrary path coverage.
const kPath = location.pathname.replace(/[^/]+$/, '');

// True when running in a document context as opposed to a worker context
const kHasDocument = typeof document !== 'undefined';

// True when running on unsecured 'http:' rather than secured 'https:'.
const kIsUnsecured = location.protocol !== 'https:';

const kCookieHelperCgi = 'resources/cookie_helper.py';

// Approximate async equivalent to the document.cookie getter but with
// important differences: optional additional getAll arguments are
// forwarded, and an empty cookie jar returns undefined.
//
// This is intended primarily for verification against expected cookie
// jar contents. It should produce more readable messages using
// assert_equals in failing cases than assert_object_equals would
// using parsed cookie jar contents and also allows expectations to be
// written more compactly.
async function getCookieString(...args) {
  const cookies = await cookieStore.getAll(...args);
  return cookies.length
    ? cookies.map(({name, value}) =>
                  (name ? (name + '=') : '') + value).join('; ')
    : undefined;
}

// Approximate async equivalent to the document.cookie getter but from
// the server's point of view. Returns UTF-8 interpretation. Allows
// sub-path to be specified.
//
// Unlike document.cookie, this returns undefined when no cookies are
// present.
async function getCookieStringHttp(extraPath = null) {
  const url =
        kCookieHelperCgi + ((extraPath == null) ? '' : ('/' + extraPath));
  const response = await fetch(url, { credentials: 'include' });
  const text = await response.text();
  assert_equals(
      response.ok,
      true,
      'CGI should have succeeded in getCookieStringHttp\n' + text);
  assert_equals(
      response.headers.get('content-type'),
      'text/plain; charset=utf-8',
      'CGI did not return UTF-8 text in getCookieStringHttp');
  if (text === '')
    return undefined;
  assert_equals(
      text.indexOf('cookie='),
      0,
      'CGI response did not begin with "cookie=" and was not empty: ' + text);
  return decodeURIComponent(text.replace(/^cookie=/, ''));
}

// Approximate async equivalent to the document.cookie getter but from
// the server's point of view. Returns binary string
// interpretation. Allows sub-path to be specified.
//
// Unlike document.cookie, this returns undefined when no cookies are
// present.
async function getCookieBinaryHttp(extraPath = null) {
  const url =
        kCookieHelperCgi +
        ((extraPath == null) ?
         '' :
         ('/' + extraPath)) + '?charset=iso-8859-1';
  const response = await fetch(url, { credentials: 'include' });
  const text = await response.text();
  assert_equals(
      response.ok,
      true,
      'CGI should have succeeded in getCookieBinaryHttp\n' + text);
  assert_equals(
      response.headers.get('content-type'),
      'text/plain; charset=iso-8859-1',
      'CGI did not return ISO 8859-1 text in getCookieBinaryHttp');
  if (text === '')
    return undefined;
  assert_equals(
      text.indexOf('cookie='),
      0,
      'CGI response did not begin with "cookie=" and was not empty: ' + text);
  return unescape(text.replace(/^cookie=/, ''));
}

// Approximate async equivalent to the document.cookie setter but from
// the server's point of view.
async function setCookieStringHttp(setCookie) {
  const encodedSetCookie = encodeURIComponent(setCookie);
  const url = kCookieHelperCgi;
  const headers = new Headers();
  headers.set(
      'content-type',
      'application/x-www-form-urlencoded; charset=utf-8');
  const response = await fetch(
      url,
      {
        credentials: 'include',
        method: 'POST',
        headers: headers,
        body: 'set-cookie=' + encodedSetCookie,
      });
  const text = await response.text();
  assert_equals(
      response.ok,
      true,
      'CGI should have succeeded in setCookieStringHttp set-cookie: ' +
        setCookie + '\n' + text);
  assert_equals(
      response.headers.get('content-type'),
      'text/plain; charset=utf-8',
      'CGI did not return UTF-8 text in setCookieStringHttp');
  assert_equals(
      text,
      'set-cookie=' + encodedSetCookie,
      'CGI did not faithfully echo the set-cookie value');
}

// Approximate async equivalent to the document.cookie setter but from
// the server's point of view. This version sets a binary cookie rather
// than a UTF-8 one.
async function setCookieBinaryHttp(setCookie) {
  const encodedSetCookie = escape(setCookie).split('/').join('%2F');
  const url = kCookieHelperCgi + '?charset=iso-8859-1';
  const headers = new Headers();
  headers.set(
      'content-type',
      'application/x-www-form-urlencoded; charset=iso-8859-1');
  const response = await fetch(url, {
    credentials: 'include',
    method: 'POST',
    headers: headers,
    body: 'set-cookie=' + encodedSetCookie
  });
  const text = await response.text();
  assert_equals(
      response.ok,
      true,
      'CGI should have succeeded in setCookieBinaryHttp set-cookie: ' +
        setCookie + '\n' + text);
  assert_equals(
      response.headers.get('content-type'),
      'text/plain; charset=iso-8859-1',
      'CGI did not return Latin-1 text in setCookieBinaryHttp');
  assert_equals(
      text,
      'set-cookie=' + encodedSetCookie,
      'CGI did not faithfully echo the set-cookie value');
}

// Async document.cookie getter; converts '' to undefined which loses
// information in the edge case where a single ''-valued anonymous
// cookie is visible.
async function getCookieStringDocument() {
  if (!kHasDocument)
    throw 'document.cookie not available in this context';
  return String(document.cookie || '') || undefined;
}

// Async document.cookie setter
async function setCookieStringDocument(setCookie) {
  if (!kHasDocument)
    throw 'document.cookie not available in this context';
  document.cookie = setCookie;
}

// Observe the next 'change' event on the cookieStore. Typical usage:
//
//   const eventPromise = observeNextCookieChangeEvent();
//   await /* something that modifies cookies */
//   await verifyCookieChangeEvent(
//     eventPromise, {changed: [{name: 'name', value: 'value'}]});
//
function observeNextCookieChangeEvent() {
  return new Promise(resolve => {
    cookieStore.addEventListener('change', e => resolve(e), {once: true});
  });
}

async function verifyCookieChangeEvent(eventPromise, expected, description) {
  description = description ? description + ': ' : '';
  expected = Object.assign({changed:[], deleted:[]}, expected);
  const event = await eventPromise;
  assert_equals(event.changed.length, expected.changed.length,
               description + 'number of changed cookies');
  for (let i = 0; i < event.changed.length; ++i) {
    assert_equals(event.changed[i].name, expected.changed[i].name,
                 description + 'changed cookie name');
    assert_equals(event.changed[i].value, expected.changed[i].value,
                 description + 'changed cookie value');
  }
  assert_equals(event.deleted.length, expected.deleted.length,
               description + 'number of deleted cookies');
  for (let i = 0; i < event.deleted.length; ++i) {
    assert_equals(event.deleted[i].name, expected.deleted[i].name,
                 description + 'deleted cookie name');
    assert_equals(event.deleted[i].value, expected.deleted[i].value,
                 description + 'deleted cookie value');
  }
}

// Helper function for promise_test with cookies; cookies
// named in these tests are cleared before/after the test
// body function is executed.
async function cookie_test(func, description) {

  // Wipe cookies used by tests before and after the test.
  async function deleteAllCookies() {
    const cookies = await cookieStore.getAll();
    await Promise.all(cookies.flatMap(
        ({name}) =>
            [cookieStore.delete(name),
             cookieStore.delete({name, partitioned: true}),
    ]));
  }

  return promise_test(async t => {
    await deleteAllCookies();
    try {
      return await func(t);
    } finally {
      await deleteAllCookies();
    }
  }, description);
}
