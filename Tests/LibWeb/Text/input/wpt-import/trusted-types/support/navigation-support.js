//                                                         lineNumber
function setWindowLocationToJavaScriptURLCode() {       // 2
// 4567890123456789 columnNumber                        // 3
  window.location = `javascript:${kJavaScriptURLCode}`; // see csp-violations.js
}

const kJavaScriptURLCode = `executeJavaScript()${';'.repeat(100)}`;

function createDefaultPolicy(defaultpolicy) {
  if (!defaultpolicy) {
    return;
  }
  trustedTypes.createPolicy("default", {
    createScript: s => {
      switch (defaultpolicy) {
      case "replace":
        return s.replace("continue", "defaultpolicywashere");
      case "replace-js-execution":
        return s.replace("executeJavaScript", "executeModifiedJavaScript");
      case "throw":
        throw new Error("Exception in createScript()");
      case "make-invalid":
        return "//make:invalid/";
      }
    },
  });
}

// Test what happens when setting window.location to a javascript: URL.
// @param defaultpolicy: a string indicating the default policy that will be
//    created before setting the location. If not specified then no default
//    policy is created.
//    - "replace-js-execution": Default policy rewrites the URL.
//    - "throw": Default policy throws an exception.
//    - "make-invalid": Default policy returns an invalid URL.
// @return an object with the following keys:
//    - exception: any exception reported by the operation.
//    - javaScriptExecuted whether the original code specified in the javascript
//      URL was executed.
//    - javaScriptExecuted whether the JavaScript code after modification by the
//      default policy was executed (for "replace-js-execution").
//    - violations: an array of reported violations for the operation.
async function setLocationToJavaScriptURL(defaultpolicy) {
  window.javaScriptExecuted = false;
  window.executeJavaScript = function() {
    window.javaScriptExecuted = true;
  }
  window.modifiedJavaScriptExecuted = false;
  window.executeModifiedJavaScript = function() {
    window.modifiedJavaScriptExecuted = true;
  }

  createDefaultPolicy(defaultpolicy);

  let {violations, exception} =
      await trusted_type_violations_and_exception_for(async _ => {
        setWindowLocationToJavaScriptURLCode();
        // Wait for the navigation to be attempted before reporting the
        // observed violations, otherwise we could miss the corresponding
        // pre-navigation check CSP violation.
        if (window.requestIdleCallback) {
          await new Promise(resolve => requestIdleCallback(resolve));
        }
      });

  return {
    exception: exception,
    javaScriptExecuted: window.javaScriptExecuted,
    modifiedJavaScriptExecuted: window.modifiedJavaScriptExecuted,
    // Clone relevant violation fields in an object, so they can be transferred
    // via cross-window via postMessage.
    violations: violations.map(violation => {
      const clonedViolation = {};
      for (const field of ["originalPolicy",
                           "violatedDirective",
                           "disposition",
                           "sample",
                           "lineNumber",
                           "columnNumber"]) {
        clonedViolation[field] = violation[field];
      }
      return clonedViolation;
    }),
  };
}

// Test what happens when navigating current page to a javascript: URL when
// clicking an anchor element, and transmit the information back to the opener.
// @param reportOnly whether the CSP rule for this page is "report-only" rather
//   than "enforce"
// The following query strings are considered:
//   - "defaultpolicy": a string indicating the default policy that will be
//      created before setting the location.
//      - "replace": Default policy replaces "continue" with
//        "defaultpolicywashere".
//      - "throw": Default policy throws an exception.
//      - "make-invalid": Default policy returns an invalid URL.
//   - "navigationattempted": whether the page was already navigated once.
//   - "frame": whether the navigation target is "frame" rather than "_self".
//   - "form-submission": navigate via a <input type="button"> element rather
//      than an <a> element.
//   - "area": navigate via an <area> element rather element rather than an <a>
//     element.
function navigateToJavascriptURL(reportOnly) {
    const params = new URLSearchParams(location.search);

    createDefaultPolicy(params.get("defaultpolicy"));

    function bounceEventToOpener(e) {
        const msg = {};
        for (const field of ["effectiveDirective", "sample", "type"]) {
            msg[field] = e[field];
        }

        msg["uri"] = location.href;
        window.opener.postMessage(msg, "*");
    }

    // If a navigation is blocked by Trusted Types, we expect this window to
    // throw a SecurityPolicyViolationEvent. If it's not blocked, we expect the
    // loaded frame to through DOMContentLoaded. In either case there should be
    // _some_ event that we can expect.
    document.addEventListener("DOMContentLoaded", bounceEventToOpener);
    // Prevent loops.
    if (params.has("navigationattempted")) {
      return;
    }

    let url = new URL(
      reportOnly ?
      // Navigate to the non-report-only version of the test. That has the same
      // event listening setup as this, but is a different target URI.
      location.href.replace("-report-only", "") :
      // We'll use a javascript:-url to navigate to ourselves, so that we can
      // re-use the messageing mechanisms above.
      location.href
    );
    url.searchParams.set("navigationattempted", 1);
    url.searchParams.set("continue", 1);
    let target_script = `location.href='${url.toString()}';`;

    function getAndPreparareNavigationElement(javaScriptURL) {
        let target = "_self";
        if (!!params.get("frame")) {
            const frame = document.createElement("iframe");
            frame.src = "frame-without-trusted-types.html";
            frames.name = "frame";
            document.body.appendChild(frame);
            target = "frame";
        }

        if (!!params.get("form-submission")) {
            const submit = document.getElementById("submit");

            // Careful, the IDL attributes are defined in camel-case.
            submit.formAction = javaScriptURL;
            submit.formTarget = target;

            return submit;
        }

        if (!!params.get("area")) {
            const area = document.getElementById("area");
            area.href = javaScriptURL;
            area.target = target;
            return area;
        }

        const anchor = document.getElementById("anchor");
        anchor.href = javaScriptURL;
        anchor.target = target;
        return anchor;
    }

    const navigationElement = getAndPreparareNavigationElement(`javascript:${target_script}`);
    document.addEventListener("DOMContentLoaded", async _ => {
      let {violations, exception} =
        await trusted_type_violations_and_exception_for(async _ => {
          navigationElement.click();
          // The timing is tricky here: we must wait for the navigation
          // to be attempted before reporting the observed violations
          // (otherwise we could miss the corresponding pre-navigation
          // check CSP violation) but we must also not wait for too
          // long, otherwise we already navigated away from the page
          // and cannot report the observed violations anymore.
          if (window.requestIdleCallback) {
            await new Promise(resolve => {
              requestIdleCallback(resolve);
              window.addEventListener("beforeunload", resolve);
            });
          }
        });
      if (exception) {
        window.opener.postMessage(`Unexpected exception: ${exception.message}`, "*");
        return;
      }
      violations.forEach(violationEvent => bounceEventToOpener(violationEvent));
      if (violations.length == 0 &&
          [null, "throw", "make-invalid"].includes(params.get("defaultpolicy"))) {
        window.opener.postMessage("No securitypolicyviolation reported!", "*");
      }
    });
}
