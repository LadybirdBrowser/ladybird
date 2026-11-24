// META: title=StorageManager API and opaque origins
// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js
// META: script=resources/helpers.js

function load_iframe(src, sandbox) {
  return new Promise(resolve => {
    const iframe = document.createElement('iframe');
    iframe.onload = () => { resolve(iframe); };
    if (sandbox)
      iframe.sandbox = sandbox;
    iframe.srcdoc = src;
    iframe.style.display = 'none';
    document.documentElement.appendChild(iframe);
  });
}

function wait_for_message(iframe) {
  return new Promise(resolve => {
    self.addEventListener('message', function listener(e) {
      if (e.source === iframe.contentWindow && "result" in e.data) {
        resolve(e.data);
        self.removeEventListener('message', listener);
      }
    });
  });
}

function make_script(snippet) {
  return '<script src="/resources/testharness.js"></script>' +
         '<script>' +
         '  window.onmessage = () => {' +
         '    try {' +
         '      (' + snippet + ')' +
         '        .then(' +
         '          result => {' +
         '            window.parent.postMessage({result: "no rejection"}, "*");' +
         '          }, ' +
         '          error => {' +
         '            try {' +
         '              assert_throws_js(TypeError, () => { throw error; });' +
         '              window.parent.postMessage({result: "correct rejection"}, "*");' +
         '            } catch (e) {' +
         '              window.parent.postMessage({result: "incorrect rejection"}, "*");' +
         '            }' +
         '          });' +
         '    } catch (ex) {' +
         // Report if not implemented/exposed, rather than time out.
         '      window.parent.postMessage({result: "API access threw"}, "*");' +
         '    }' +
         '  };' +
         '<\/script>';
}

promise_setup(async () => {
  await tryDenyingPermission();
});

['navigator.storage.persisted()',
 'navigator.storage.estimate()',
 // persist() can prompt, so make sure we test that last
 'navigator.storage.persist()',
].forEach(snippet => {
  promise_test(t => {
    return load_iframe(make_script(snippet))
      .then(iframe => {
        iframe.contentWindow.postMessage({}, '*');
        return wait_for_message(iframe);
      })
      .then(message => {
        assert_equals(message.result, 'no rejection',
                      `${snippet} should not reject`);
      });
  }, `${snippet} in non-sandboxed iframe should not reject`);

  promise_test(t => {
    return load_iframe(make_script(snippet), 'allow-scripts')
      .then(iframe => {
        iframe.contentWindow.postMessage({}, '*');
        return wait_for_message(iframe);
      })
      .then(message => {
        assert_equals(message.result, 'correct rejection',
                      `${snippet} should reject with TypeError`);
      });
  }, `${snippet} in sandboxed iframe should reject with TypeError`);
});
