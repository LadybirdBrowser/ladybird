// META: title=Sandboxed iframe top window close
// META: script=/common/get-host-info.sub.js

'use strict';

const IFRAME_PATH = '/html/semantics/embedded-content/the-iframe-element/resources/sandbox-top-close-iframe.html';

function testWithSandboxAttr(sandbox, xorigin = false) {
  return new Promise(resolve => {
    const popup = window.open('about:blank');

    window.addEventListener('message', function handler(e) {
      if (e.data !== 'done') return;
      window.removeEventListener('message', handler);
      const closed = popup.closed;
      if (!closed) popup.close();
      resolve(closed);
    });

    const iframe = popup.document.createElement('iframe');
    iframe.sandbox = sandbox;
    if (xorigin) {
      iframe.src = get_host_info().HTTP_REMOTE_ORIGIN + IFRAME_PATH;
    } else {
      iframe.src = IFRAME_PATH;
    }
    popup.document.body.appendChild(iframe);
  });
}

promise_test(async () => {
  const closed = await testWithSandboxAttr('allow-scripts');
  assert_false(closed);
}, 'Sandboxed iframe without allow-top-navigation cannot close top window');

promise_test(async () => {
  const closed = await testWithSandboxAttr('allow-scripts allow-top-navigation');
  assert_true(closed);
}, 'Sandboxed iframe with allow-top-navigation can close top window');

promise_test(async () => {
  const closed = await testWithSandboxAttr('allow-scripts', true);
  assert_false(closed);
}, 'Cross-origin sandboxed iframe without allow-top-navigation cannot close top window');
