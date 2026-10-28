// Feature test to avoid timeouts
function assert_permissions_policy_supported() {
  assert_true("allow" in HTMLIFrameElement.prototype, 'permissions policy is supported');
}
// Tests whether a feature that is enabled/disabled by permissions policy works
// as expected.
// Arguments:
//    feature_descriptionOrObject: either and object, containing the following
//        properties, or a string describing what feature is being tested.
//        Examples: "usb.GetDevices()", "PaymentRequest()".
//    test: test created by testharness. Examples: async_test, promise_test.
//    src: URL where a feature's availability is checked. Examples:
//        "/permissions-policy/resources/permissions-policy-payment.html",
//        "/permissions-policy/resources/permissions-policy-usb.html".
//    expect_feature_available: a callback(data, feature_description) to
//        verify if a feature is available or unavailable as expected.
//        The file under the path "src" defines what "data" is sent back via
//        postMessage with type: 'availability-result'.
//        Inside the callback, some tests (e.g., EXPECT_EQ, EXPECT_TRUE, etc)
//        are run accordingly to test a feature's availability.
//        Example: expect_feature_available_default(data, feature_description).
//    feature_name: Optional argument, only provided when testing iframe allow
//      attribute. "feature_name" is the feature name of a policy controlled
//      feature (https://w3c.github.io/webappsec-permissions-policy/#features).
//      See examples at:
//      https://github.com/w3c/webappsec-permissions-policy/blob/main/features.md
//    allowfullscreen: Optional argument, only used for testing fullscreen
//      by passing "allowfullscreen".
//    is_promise_test: Optional argument, true if this call should return a
//    promise. Used by test_feature_availability_with_post_message_result()
function test_feature_availability(
    feature_descriptionOrObject, test, src, expect_feature_available, feature_name,
    allowfullscreen, is_promise_test = false, needs_focus = false) {

  if (feature_descriptionOrObject && feature_descriptionOrObject instanceof Object) {
    const {
      feature_description,
      test,
      src,
      expect_feature_available,
      feature_name,
      allowfullscreen,
      is_promise_test,
      needs_focus,
    } = feature_descriptionOrObject;
    return test_feature_availability(
      feature_description,
      test,
      src,
      expect_feature_available,
      feature_name,
      allowfullscreen,
      is_promise_test,
      needs_focus,
    );
  }

  const feature_description = feature_descriptionOrObject;
  let frame = document.createElement('iframe');
  frame.src = src;

  if (typeof feature_name !== 'undefined') {
    frame.allow = frame.allow.concat(";" + feature_name);
  }

  if (typeof allowfullscreen !== 'undefined') {
    frame.setAttribute(allowfullscreen, true);
  }

  function expectFeatureAvailable(evt) {
    if (evt.source === frame.contentWindow &&
        evt.data.type === 'availability-result') {
      expect_feature_available(evt.data, feature_description);
      document.body.removeChild(frame);
      test.done();
    }
  }

  if (!is_promise_test) {
    window.addEventListener('message', test.step_func(expectFeatureAvailable));
    document.body.appendChild(frame);
    return;
  }

  const promise = new Promise((resolve) => {
                    window.addEventListener('message', resolve);
                  }).then(expectFeatureAvailable);
  document.body.appendChild(frame);
  if (needs_focus) {
    frame.focus();
  }
  return promise;
}

// Default helper functions to test a feature's availability:
function expect_feature_available_default(data, feature_description) {
  assert_true(data.enabled, feature_description);
}

function expect_feature_unavailable_default(data, feature_description) {
  assert_false(data.enabled, feature_description);
}

// This is the same as test_feature_availability() but instead of passing in a
// function to check the result of the message sent back from an iframe, instead
// just compares the result to an expected result passed in.
// Arguments:
//     test: test created by testharness. Examples: async_test, promise_test.
//     src: the URL to load in an iframe in which to test the feature.
//     expected_result: the expected value to compare to the data passed back
//         from the src page by postMessage.
//     allow_attribute: Optional argument, only provided when an allow
//         attribute should be specified on the iframe.
function test_feature_availability_with_post_message_result(
    test, src, expected_result, allow_attribute) {
  const test_result = ({ name, message }, feature_description) => {
    assert_equals(name, expected_result, message + '.');
  };
  return test_feature_availability(
      null, test, src, test_result, allow_attribute, undefined, true);
}

// If this page is intended to test the named feature (according to the URL),
// tests the feature availability and posts the result back to the parent.
// Otherwise, does nothing.
async function test_feature_in_iframe(feature_name, feature_promise_factory) {
  if (location.hash.endsWith(`#${feature_name}`)) {
    let message = 'Available';
    let name = '#OK';
    try {
      await feature_promise_factory();
    } catch (e) {
      ({ name, message } = e);
    }
    window.parent.postMessage(
      { type: 'availability-result', name, message }, '*');
  }
}

// Returns true if the URL for this page indicates that it is embedded in an
// iframe.
function page_loaded_in_iframe() {
  return new URLSearchParams(location.search).get('in-iframe');
}

// Returns a same-origin (relative) URL suitable for embedding in an iframe for
// testing the availability of the feature.
function same_origin_url(feature_name) {
  // Add an "in-iframe" query parameter so that we can detect the iframe'd
  // version of the page and testharness script loading can be disabled in
  // that version, as required for use of testdriver in non-toplevel browsing
  // contexts.
  return location.pathname + '?in-iframe=yes#' + feature_name;
}

// Returns a cross-origin (absolute) URL suitable for embedding in an iframe for
// testing the availability of the feature.
function cross_origin_url(base_url, feature_name) {
  return base_url + same_origin_url(feature_name);
}

// This function runs all permissions policy tests for a particular feature that
// has a default policy of "self". This includes testing:
// 1. Feature usage succeeds by default in the top level frame.
// 2. Feature usage succeeds by default in a same-origin iframe.
// 3. Feature usage fails by default in a cross-origin iframe.
// 4. Feature usage succeeds when an allow attribute is specified on a
//    cross-origin iframe.
// 5. Feature usage fails when an allow attribute is specified on a
//    same-origin iframe with a value of "feature-name 'none'".
//
// The same page which called this function will be loaded in the iframe in
// order to test feature usage there. When this function is called in that
// context it will simply run the feature and return a result back via
// postMessage.
//
// Arguments:
//     cross_origin: A cross-origin URL base to be used to load the page which
//         called into this function.
//     feature_name: The name of the feature as it should be specified in an
//         allow attribute.
//     error_name: If feature usage does not succeed, this is the string
//         representation of the error that will be passed in the rejected
//         promise.
//     feature_promise_factory: A function which returns a promise which tests
//         feature usage. If usage succeeds, the promise should resolve. If it
//         fails, the promise should reject with an error that can be
//         represented as a string.
function run_all_fp_tests_allow_self(
    cross_origin, feature_name, error_name, feature_promise_factory) {
  // This may be the version of the page loaded up in an iframe. If so, just
  // post the result of running the feature promise back to the parent.
  if (page_loaded_in_iframe()) {
    test_feature_in_iframe(feature_name, feature_promise_factory);
    return;
  }

  // Run the various tests.
  // 1. Allowed in top-level frame.
  promise_test(
      () => feature_promise_factory(),
      'Default "' + feature_name +
          '" permissions policy ["self"] allows the top-level document.');

  // 2. Allowed in same-origin iframe.
  const same_origin_frame_pathname = same_origin_url(feature_name);
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, same_origin_frame_pathname, '#OK');
      },
      'Default "' + feature_name +
          '" permissions policy ["self"] allows same-origin iframes.');

  // 3. Blocked in cross-origin iframe.
  const cross_origin_frame_url = cross_origin_url(cross_origin, feature_name);
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, cross_origin_frame_url, error_name);
      },
      'Default "' + feature_name +
          '" permissions policy ["self"] disallows cross-origin iframes.');

  // 4. Allowed in cross-origin iframe with "allow" attribute.
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, cross_origin_frame_url, '#OK', feature_name);
      },
      'permissions policy "' + feature_name +
          '" can be enabled in cross-origin iframes using "allow" attribute.');

  // 5. Blocked in same-origin iframe with "allow" attribute set to 'none'.
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, same_origin_frame_pathname, error_name,
            feature_name + ' \'none\'');
      },
      'permissions policy "' + feature_name +
          '" can be disabled in same-origin iframes using "allow" attribute.');
}

// This function runs all permissions policy tests for a particular feature that
// has a default policy of "*". This includes testing:
// 1. Feature usage succeeds by default in the top level frame.
// 2. Feature usage succeeds by default in a same-origin iframe.
// 3. Feature usage succeeds by default in a cross-origin iframe.
// 4. Feature usage fails when an allow attribute is specified on a
//    cross-origin iframe with a value of "feature-name 'none'".
// 5. Feature usage fails when an allow attribute is specified on a
//    same-origin iframe with a value of "feature-name 'none'".
//
// The same page which called this function will be loaded in the iframe in
// order to test feature usage there. When this function is called in that
// context it will simply run the feature and return a result back via
// postMessage.
//
// Arguments:
//     cross_origin: A cross-origin URL base to be used to load the page which
//         called into this function.
//     feature_name: The name of the feature as it should be specified in an
//         allow attribute.
//     error_name: If feature usage does not succeed, this is the string
//         representation of the error that will be passed in the rejected
//         promise.
//     feature_promise_factory: A function which returns a promise which tests
//         feature usage. If usage succeeds, the promise should resolve. If it
//         fails, the promise should reject with an error that can be
//         represented as a string.
function run_all_fp_tests_allow_all(
    cross_origin, feature_name, error_name, feature_promise_factory) {
  // This may be the version of the page loaded up in an iframe. If so, just
  // post the result of running the feature promise back to the parent.
  if (page_loaded_in_iframe()) {
    test_feature_in_iframe(feature_name, feature_promise_factory);
    return;
  }

  // Run the various tests.
  // 1. Allowed in top-level frame.
  promise_test(
      () => feature_promise_factory(),
      'Default "' + feature_name +
          '" permissions policy ["*"] allows the top-level document.');

  // 2. Allowed in same-origin iframe.
  const same_origin_frame_pathname = same_origin_url(feature_name);
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, same_origin_frame_pathname, '#OK');
      },
      'Default "' + feature_name +
          '" permissions policy ["*"] allows same-origin iframes.');

  // 3. Allowed in cross-origin iframe.
  const cross_origin_frame_url = cross_origin_url(cross_origin, feature_name);
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, cross_origin_frame_url, '#OK');
      },
      'Default "' + feature_name +
          '" permissions policy ["*"] allows cross-origin iframes.');

  // 4. Blocked in cross-origin iframe with "allow" attribute set to 'none'.
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, cross_origin_frame_url, error_name, feature_name + ' \'none\'');
      },
      'permissions policy "' + feature_name +
          '" can be disabled in cross-origin iframes using "allow" attribute.');

  // 5. Blocked in same-origin iframe with "allow" attribute set to 'none'.
  promise_test(
      t => {
        return test_feature_availability_with_post_message_result(
            t, same_origin_frame_pathname, error_name,
            feature_name + ' \'none\'');
      },
      'permissions policy "' + feature_name +
          '" can be disabled in same-origin iframes using "allow" attribute.');
}

// This function tests that a subframe's document policy allows a given feature.
// A feature is allowed in a frame either through inherited policy or specified
// by iframe allow attribute.
// Arguments:
//     test: test created by testharness. Examples: async_test, promise_test.
//     feature: feature name that should be allowed in the frame.
//     src: the URL to load in the frame.
//     allow: the allow attribute (container policy) of the iframe
function test_allowed_feature_for_subframe(message, feature, src, allow) {
  let frame = document.createElement('iframe');
  if (typeof allow !== 'undefined') {
    frame.allow = allow;
  }
  promise_test(function() {
    assert_permissions_policy_supported();
    frame.src = src;
    return new Promise(function(resolve, reject) {
      window.addEventListener('message', function handler(evt) {
        resolve(evt.data);
      }, { once: true });
      document.body.appendChild(frame);
    }).then(function(data) {
      assert_true(data.includes(feature), feature);
    });
  }, message);
}

// This function tests that a subframe's document policy disallows a given
// feature. A feature is allowed in a frame either through inherited policy or
// specified by iframe allow attribute.
// Arguments:
//     test: test created by testharness. Examples: async_test, promise_test.
//     feature: feature name that should not be allowed in the frame.
//     src: the URL to load in the frame.
//     allow: the allow attribute (container policy) of the iframe
function test_disallowed_feature_for_subframe(message, feature, src, allow) {
  let frame = document.createElement('iframe');
  if (typeof allow !== 'undefined') {
    frame.allow = allow;
  }
  promise_test(function() {
    assert_permissions_policy_supported();
    frame.src = src;
    return new Promise(function(resolve, reject) {
      window.addEventListener('message', function handler(evt) {
        resolve(evt.data);
      }, { once: true });
      document.body.appendChild(frame);
    }).then(function(data) {
      assert_false(data.includes(feature), feature);
    });
  }, message);
}

// This function tests that a subframe with header policy defined on a given
// feature allows and disallows the feature as expected.
// Arguments:
//     feature: feature name.
//     frame_header_policy: either *, self or \\(\\), defines the frame
//                          document's header policy on |feature|.
//                          '(' and ')' need to be escaped because of server end
//                          header parameter syntax limitation.
//     src: the URL to load in the frame.
//     test_expects: contains 6 expected results of either |feature| is allowed
//                   or not inside of a local or remote iframe nested inside
//                   the subframe given the header policy to be either *,
//                   self, or ().
//     test_name: name of the test.
function test_subframe_header_policy(
    feature, frame_header_policy, src, test_expects, test_name) {
  let frame = document.createElement('iframe');
  promise_test(function() {
    assert_permissions_policy_supported()
    frame.src = src + '?pipe=sub|header(Permissions-Policy,' + feature + '='
        + frame_header_policy + ')';
    return new Promise(function(resolve) {
      window.addEventListener('message', function handler(evt) {
        resolve(evt.data);
      });
      document.body.appendChild(frame);
    }).then(function(results) {
      for (var j = 0; j < results.length; j++) {
        var data = results[j];

        function test_result(message, test_expect) {
          if (test_expect) {
            assert_true(data.allowedfeatures.includes(feature), message);
          } else {
            assert_false(data.allowedfeatures.includes(feature), message);
          }
        }

        if (data.frame === 'local') {
          if (data.policy === '*') {
            test_result('local_all:', test_expects.local_all);
          }
          if (data.policy === 'self') {
            test_result('local_self:', test_expects.local_self);
          }
          if (data.policy === '\\(\\)') {
            test_result('local_none:', test_expects.local_none);
          }
        }

        if (data.frame === 'remote') {
          if (data.policy === '*') {
            test_result('remote_all:', test_expects.remote_all);
          }
          if (data.policy === 'self') {
            test_result('remote_self:', test_expects.remote_self);
          }
          if (data.policy === '\\(\\)') {
            test_result('remote_none:', test_expects.remote_none);
          }
        }
      }
    });
  }, test_name);
}

// This function tests that frame policy allows a given feature correctly. A
// feature is allowed in a frame either through inherited policy or specified
// by iframe allow attribute.
// Arguments:
//     feature: feature name.
//     src: the URL to load in the frame. If undefined, the iframe will have a
//         srcdoc="" attribute
//     test_expect: boolean value of whether the feature should be allowed.
//     allow: optional, the allow attribute (container policy) of the iframe.
//     allowfullscreen: optional, boolean value of allowfullscreen attribute.
//     sandbox: optional boolean. If true, the frame will be sandboxed (with
//         allow-scripts, so that tests can run in it.)
function test_frame_policy(
    feature, src, srcdoc, test_expect, allow, allowfullscreen, sandbox) {
  let frame = document.createElement('iframe');
  document.body.appendChild(frame);
  // frame_policy should be dynamically updated as allow and allowfullscreen is
  // updated.
  var frame_policy = frame.permissionsPolicy;
  if (typeof allow !== 'undefined') {
    frame.setAttribute('allow', allow);
  }
  if (!!allowfullscreen) {
    frame.setAttribute('allowfullscreen', true);
  }
  if (!!sandbox) {
    frame.setAttribute('sandbox', 'allow-scripts');
  }
  if (!!src) {
    frame.src = src;
  }
  if (!!srcdoc) {
    frame.srcdoc = "<h1>Hello world!</h1>";
  }
  if (test_expect) {
    assert_true(frame_policy.allowedFeatures().includes(feature));
  } else {
    assert_false(frame_policy.allowedFeatures().includes(feature));
  }
}

function expect_reports(report_count, policy_name, description) {
  async_test(t => {
    var num_received_reports = 0;
    new ReportingObserver(t.step_func((reports, observer) => {
        const relevant_reports = reports.filter(r => (r.body.featureId === policy_name));
        num_received_reports += relevant_reports.length;
        if (num_received_reports >= report_count) {
            t.done();
        }
   }), {types: ['permissions-policy-violation'], buffered: true}).observe();
  }, description);
}
