// META: title=Cookie Store API: Test that changing only a cookie's attributes fires an event.
// META: script=resources/cookie-test-helpers.js

'use strict';

// Changing a cookie is web-observable even when its value stays the same. These
// complement change_eventhandler_for_no_change.https.window.js, which covers a
// cookie whose value and attributes are all unchanged.
//
// See https://github.com/httpwg/http-extensions/issues/3501.
//
// HttpOnly is not covered here as it cannot be set through CookieStore or
// document.cookie; cookies/attributes/httponly-overwrite.https.html covers it
// at the level of the cookie store instead.

cookie_test(async t => {
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    expires: new Date(Date.now() + 100_000_000),
  });

  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    expires: new Date(Date.now() + 200_000_000),
  });
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Change of expiry time is observed');
}, 'CookieStore change of expires only should be observed');

cookie_test(async t => {
  await cookieStore.set({name: 'cookie', value: 'VALUE', sameSite: 'strict'});

  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({name: 'cookie', value: 'VALUE', sameSite: 'lax'});
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Change of same-site is observed');
}, 'CookieStore change of sameSite only should be observed');

// CookieStore always sets Secure, so document.cookie is used to vary it.
cookie_test(async t => {
  await setCookieStringDocument('cookie=VALUE; path=/; secure');

  const eventPromise = observeNextCookieChangeEvent();
  await setCookieStringDocument('cookie=VALUE; path=/');
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Change of secure is observed');
}, 'document.cookie change of Secure only should be observed');

// An expiry time of null (a session cookie) and an expiry time in the future
// are distinct, in both directions.
cookie_test(async t => {
  await cookieStore.set({name: 'cookie', value: 'VALUE'});

  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    expires: new Date(Date.now() + 100_000_000),
  });
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Gaining an expiry time is observed');
}, 'CookieStore change from no expires to expires should be observed');

cookie_test(async t => {
  await cookieStore.set({
    name: 'cookie',
    value: 'VALUE',
    expires: new Date(Date.now() + 100_000_000),
  });

  const eventPromise = observeNextCookieChangeEvent();
  await cookieStore.set({name: 'cookie', value: 'VALUE'});
  await verifyCookieChangeEvent(
    eventPromise, {changed: [{name: 'cookie', value: 'VALUE'}]},
    'Losing an expiry time is observed');
}, 'CookieStore change from expires to no expires should be observed');
