setup({allow_uncaught_exception : true});

// Creates a new Document (via <iframe>) and add an inline import map.
function createTestIframe(importMap, importMapBaseURL) {
  return new Promise(resolve => {
    const iframe = document.createElement('iframe');

    window.addEventListener('message', event => {
        // Parsing result is saved here and checked later, rather than
        // rejecting the promise on errors.
        iframe.parseImportMapResult = event.data.type;
        resolve(iframe);
      },
      {once: true});

    const testHTML = createTestHTML(importMap, importMapBaseURL);

    if (new URL(importMapBaseURL).protocol === 'data:') {
      iframe.src = 'data:text/html;base64,' + btoa(testHTML);
    } else {
      iframe.src = '/common/blank.html';
      iframe.addEventListener('load', () => {
        iframe.contentDocument.write(testHTML);
        iframe.contentDocument.close();
      }, {once: true});
    }
    document.body.appendChild(iframe);
  });
}

function createTestHTML(importMap, importMapBaseURL) {
  return `
    <!DOCTYPE html>
    <script src="${location.origin}/import-maps/data-driven/resources/test-helper-iframe.js"></script>

    <base href="${importMapBaseURL}">
    <script type="importmap" onerror="onScriptError(event)">
    ${JSON.stringify(importMap)}
    </script>

    <script type="module">
      if (!window.registrationResult) {
        window.registrationResult = {type: 'Success'};
      }
      window.removeEventListener('error', window.windowErrorHandler);
      parent.postMessage(window.registrationResult, '*');
    </script>
  `;
}

// Returns a promise that is resolved with the resulting URL, or rejected if
// the resolution fails.
function resolve(specifier, baseURL, iframe) {
  return new Promise((resolve, reject) => {
    window.addEventListener('message', event => {
        if (event.data.type === 'ResolutionSuccess') {
          resolve(event.data.result);
        } else if (event.data.type === 'Failure') {
          if (event.data.result === 'TypeError') {
            reject(new TypeError(event.data.message));
          } else {
            reject(new Error(event.data.message));
          }
        } else {
          assert_unreached('Invalid message: ' + event.data.type);
        }
      },
      {once: true});

    iframe.contentWindow.postMessage(
      {action: 'resolve', specifier, baseURL},
      '*'
    );
  });
}

function assert_no_extra_properties(object, expectedProperties, description) {
  for (const actualProperty in object) {
    assert_true(expectedProperties.indexOf(actualProperty) !== -1,
        description + ': unexpected property ' + actualProperty);
  }
}

async function runTests(j) {
  const tests = j.tests;
  delete j.tests;

  if (j.hasOwnProperty('importMap')) {
    assert_own_property(j, 'importMap');
    assert_own_property(j, 'importMapBaseURL');
    j.iframe = await createTestIframe(j.importMap, j.importMapBaseURL);
    delete j.importMap;
    delete j.importMapBaseURL;
  }

  assert_no_extra_properties(
      j,
      ['expectedResults', 'expectedParsedImportMap',
      'baseURL', 'name', 'iframe',
      'importMap', 'importMapBaseURL',
      'link', 'details'],
      j.name);

  if (tests) {
    // Nested node.
    for (const testName in tests) {
      let fullTestName = testName;
      if (j.name) {
        fullTestName = j.name + ': ' + testName;
      }
      tests[testName].name = fullTestName;
      const k = Object.assign({}, j, tests[testName]);
      await runTests(k);
    }
  } else {
    // Leaf node.
    for (const key of ['iframe', 'name', 'expectedResults']) {
      assert_own_property(j, key, j.name);
    }

    assert_equals(
        j.iframe.parseImportMapResult,
        'Success',
        'Import map registration should be successful for resolution tests');
    for (const [specifier, expected] of Object.entries(j.expectedResults)) {
      promise_test(async t => {
        if (expected === null) {
          return promise_rejects_js(t, TypeError, resolve(specifier, j.baseURL, j.iframe));
        } else {
          assert_equals(await resolve(specifier, j.baseURL, j.iframe), expected);
        }
      },
      j.name + ': ' + specifier);
    }
  }
}

export async function runTestsFromJSON(jsonURL) {
  const response = await fetch(jsonURL);
  const json = await response.json();
  await runTests(json);
}
