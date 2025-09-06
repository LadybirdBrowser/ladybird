const cspDirectives = [
  // https://w3c.github.io/trusted-types/dist/spec/#require-trusted-types-for-csp-directive
  "require-trusted-types-for",
  // https://w3c.github.io/trusted-types/dist/spec/#trusted-types-csp-directive
  "trusted-types",
  // https://w3c.github.io/webappsec-csp/#script-src
  "script-src",
  // https://w3c.github.io/webappsec-csp/#directive-script-src-elem
  "script-src-elem",
];

// A generic helper that runs function fn and returns a promise resolving with
// an array of reported violations and a possible exception thrown. This forces
// a "connect-src" violation before and after calling fn, to make sure we are
// not gathering violations reported before fn, and that all the violations
// reported by fn have been delivered. This assumes that the test file contains
// the CSP directive connect-src 'none' and that fn is not itself triggering
// a "connect-src" violation report.
function trusted_type_violations_and_exception_for(fn) {
  return new Promise(async (resolve, reject) => {
    // Listen for security policy violations.
    let result = { violations: [], exception: null };
    let handler = e => {
      if (cspDirectives.includes(e.effectiveDirective)) {
        result.violations.push(e);
      } else if (e.effectiveDirective === "connect-src") {
        self.removeEventListener("securitypolicyviolation", handler);
        e.stopPropagation();
        resolve(result);
      } else {
        reject(`Unexpected violation for directive ${e.effectiveDirective}`);
      }
    }
    self.addEventListener("securitypolicyviolation", handler);

    // Run the specified function and record any exception.
    try {
      await fn();
    } catch(e) {
      result.exception = e;
    }
    // Force a connect-src violation. WebKit additionally throws a SecurityError
    // so ignore that. See https://bugs.webkit.org/show_bug.cgi?id=286744
    // Firefox throws a NS_ERROR_CONTENT_BLOCKED exception, so ignore that too.
    // See https://bugzilla.mozilla.org/show_bug.cgi?id=1951698
    try {
      // FIXME: Out Websocket implementation is not yet hooked to our fetch apis and does not emmit any violations.
      // This results in test timing out. We can achieve the same result with a simple fetch call
      fetch("../common/blank.html")
      //new WebSocket("ws:/common/blank.html");
    } catch(e) {
      if ((!e instanceof DOMException || e.name !== "SecurityError") && e.name !== "NS_ERROR_CONTENT_BLOCKED") {
        throw e;
      }
    }
  });
}

// Helper function when we expect one violation and exception.
async function trusted_type_violation_for(expectedException, fn) {
  let {violations, exception} =
      await trusted_type_violations_and_exception_for(fn);
  assert_equals(violations.length, 1, "a single violation reported");
  assert_true(exception instanceof expectedException, `${expectedException.prototype} exception reported`);
  return violations[0];
}

// Helper function when we expect no violation or exception.
async function no_trusted_type_violation_for(fn) {
  let {violations, exception} =
      await trusted_type_violations_and_exception_for(fn);
  assert_equals(violations.length, 0, "no violation reported");
  assert_equals(exception, null, "no exception thrown");
}

async function trusted_type_violation_without_exception_for(fn) {
  let {violations, exception} =
      await trusted_type_violations_and_exception_for(fn);
  assert_equals(violations.length, 1, "a single violation reported");
  assert_equals(exception, null, "no exception thrown");
  return violations[0];
}

function clipSampleIfNeeded(sample) {
  const clippedSampleLength = 40;

  // Clipping is a bit ambiguous when the sample contains surrogate pairs, so
  // avoid that in our tests for now.
  // https://github.com/w3c/trusted-types/issues/577
  assert_equals(sample.match(/[\uD800-\uDBFF][\uDC00-\uDFFF]/), null);

  return sample.substring(0, clippedSampleLength);
}

function tryCreatingTrustedTypePoliciesWithCSP(policyNames, cspHeaders) {
  return new Promise(resolve => {
    window.addEventListener("message", event => {
      resolve(event.data);
    }, {once: true});
    let iframe = document.createElement("iframe");
    let url = `/trusted-types/support/create-trusted-type-policies.html?policyNames=${policyNames.map(name => encodeURIComponent(name)).toString()}`;
    url += "&pipe=header(Content-Security-Policy,connect-src 'none')"
    if (cspHeaders)
      url += `|${cspHeaders}`;
    iframe.src = url;
    document.head.appendChild(iframe);
  });
}

function trySendingPlainStringToTrustedTypeSink(sinkGroups, cspHeaders) {
  return new Promise(resolve => {
    window.addEventListener("message", event => {
      resolve(event.data);
    }, {once: true});
    let iframe = document.createElement("iframe");
    let url = `/trusted-types/support/send-plain-string-to-trusted-type-sink.html?sinkGroups=${sinkGroups.map(name => encodeURIComponent(name)).toString()}`;
    url += "&pipe=header(Content-Security-Policy,connect-src 'none')"
    if (cspHeaders)
      url += `|${cspHeaders}`;
    iframe.src = url;
    document.head.appendChild(iframe);
  });
}

function tryNavigatingToJavaScriptURLInSubframe(cspHeaders) {
  return new Promise(resolve => {
    window.addEventListener("message", event => {
      resolve(event.data);
    }, {once: true});
    let iframe = document.createElement("iframe");
    let url = "/trusted-types/support/navigate-to-javascript-url.html";
    url += "?pipe=header(Content-Security-Policy,connect-src 'none')"
    if (cspHeaders)
      url += `|${cspHeaders}`;
    iframe.src = url;
    document.head.appendChild(iframe);
  });
}
