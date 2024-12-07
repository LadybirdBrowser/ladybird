/* Utilities related to WAI-ARIA */

const AriaUtils = {

  /*
  Tests simple role assignment: <div role="alert">x</div>
  Not intended for nested, context-dependent, or other complex role tests.

  Ex: AriaUtils.assignAndVerifyRolesByRoleNames(["group", "main", "button"])

  */
  assignAndVerifyRolesByRoleNames: function(roleNames) {
    if (!Array.isArray(roleNames) || !roleNames.length) {
      throw `Param roleNames of assignAndVerifyRolesByRoleNames("${roleNames}") should be an array containing at least one role string.`;
    }
    for (const role of roleNames) {
      promise_test(async t => {
        let el = document.createElement("div");
        el.appendChild(document.createTextNode("x"));
        el.setAttribute("role", role); // el.role not yet supported by Gecko.
        document.body.appendChild(el);
        const computedRole = await test_driver.get_computed_role(el);
        assert_equals(computedRole, role, el.outerHTML);
      }, `role: ${role}`);
    }
  },


  /*
  Tests computed ROLE of all elements matching selector
  against the string value of their data-expectedrole attribute.

  Ex: <div role="list"
        data-testname="optional unique test name"
        data-expectedrole="list"
        class="ex">

      AriaUtils.verifyRolesBySelector(".ex")

  */
  verifyRolesBySelector: function(selector, roleTestNamePrefix) {
    const els = document.querySelectorAll(selector);
    if (!els.length) {
      throw `Selector passed in verifyRolesBySelector("${selector}") should match at least one element.`;
    }
    for (const el of els) {
      let role = el.getAttribute("data-expectedrole");
      let testName = el.getAttribute("data-testname") || role; // data-testname optional if role is unique per test file
      if (typeof roleTestNamePrefix !== "undefined") {
        testName = roleTestNamePrefix + testName;
      }
      promise_test(async t => {
        const expectedRole = el.getAttribute("data-expectedrole");
        const computedRole = await test_driver.get_computed_role(el);
        assert_equals(computedRole, expectedRole, el.outerHTML);
      }, `${testName}`);
    }
  },


  /*
  Tests computed ROLE of selected elements matching selector
  against the string value of provided roles array.

  Ex: <foo
        data-testname="verify fooRole or barRole role on span"
        class="ex-foo-or-bar">

      AriaUtils.verifyRoleOrVariantRolesBySelector(".ex-foo-or-bar", ["fooRole", "barRole"]);

  See also helper function verifyGenericRolesBySelector shorthand of the above using ["generic", "", "none"].

  Note: This function should not be used to circumvent unexpected interop differences in implementations.
  It should only be used in specific cases (like "generic") determined by ARIA WG or other spec maintainers to be acceptable for the purposes of testing.

  */
  verifyRoleOrVariantRolesBySelector: function(selector, roles) {
    const els = document.querySelectorAll(selector);
    if (!els.length) {
      throw `Selector "${selector}" should match at least one element.`;
    }
    if (!roles.length || roles.length < 2) {
      throw `Roles array ["${roles.join('", "')}"] should include at least two strings, a primary role and at least one acceptable implementation-specific variant. E.g. ["generic", "", "none"]…`;
    }
    for (const el of els) {
      let testName = el.getAttribute("data-testname");
      promise_test(async t => {
        const expectedRoles = roles;
        const computedRole = await test_driver.get_computed_role(el);
        for (role of roles){
          if (computedRole === role) {
            return assert_equals(computedRole, role, `Computed Role: "${computedRole}" matches one of the acceptable role strings in ["${roles.join('", "')}"]: ${el.outerHTML}`);
          }
        }
        return assert_false(true, `Computed Role: "${computedRole}" does not match any of the acceptable role strings in ["${roles.join('", "')}"]: ${el.outerHTML}`);
      }, `${testName}`);
    }
  },


  /*
  Helper function for "generic" ROLE tests.

  Ex: <span
        data-testname="verify generic, none, or empty computed role on span"
        class="ex-generic">

      AriaUtils.verifyGenericRolesBySelector(".ex-generic");

   This helper function is equivalant to AriaUtils.verifyRoleOrVariantRolesBySelector(".ex-generic", ["generic", "", "none"]);
   See various issues and discussions linked from https://github.com/web-platform-tests/interop-accessibility/issues/48

  */
  verifyGenericRolesBySelector: function(selector) {
    // ARIA WG determined implementation variants "none" (Chromium), and the empty string "" (WebKit), are sufficiently equivalent to "generic" for WPT test verification of HTML-AAM.
    // See various discussions linked from https://github.com/web-platform-tests/interop-accessibility/issues/48
    this.verifyRoleOrVariantRolesBySelector(selector, ["generic", "", "none"]);
  },


  /*
  Tests computed LABEL of all elements matching selector
  against the string value of their data-expectedlabel attribute.

  Ex: <div aria-label="foo"
        data-testname="optional unique test name"
        data-expectedlabel="foo"
        class="ex">

      AriaUtils.verifyLabelsBySelector(".ex")

  */
  verifyLabelsBySelector: function(selector, labelTestNamePrefix) {
    const els = document.querySelectorAll(selector);
    if (!els.length) {
      throw `Selector passed in verifyLabelsBySelector("${selector}") should match at least one element.`;
    }
    for (const el of els) {
      let label = el.getAttribute("data-expectedlabel");
      let testName = el.getAttribute("data-testname") || label; // data-testname optional if label is unique per test file
      if (typeof labelTestNamePrefix !== "undefined") {
        testName = labelTestNamePrefix + testName;
      }
      promise_test(async t => {
        const expectedLabel = el.getAttribute("data-expectedlabel");
        let computedLabel = await test_driver.get_computed_label(el);
        assert_not_equals(computedLabel, null, `get_computed_label(el) shouldn't return null for ${el.outerHTML}`);

        // See:
        // - https://github.com/w3c/accname/pull/165
        // - https://github.com/w3c/accname/issues/192
        // - https://github.com/w3c/accname/issues/208
        //
        // AccName references HTML's definition of ASCII Whitespace
        // https://infra.spec.whatwg.org/#ascii-whitespace
        // which matches tab (\t), newline (\n), formfeed (\f), return (\r), and regular space (\u0020).
        // but it does NOT match non-breaking space (\xA0,\u00A0) and others matched by \s
        const asciiWhitespace = /[\t\n\f\r\u0020]+/g;
        computedLabel = computedLabel.replace(asciiWhitespace, '\u0020').replace(/^\u0020|\u0020$/g, '');

        assert_equals(computedLabel, expectedLabel, el.outerHTML);
      }, `${testName}`);
    }
  },


  /*
  Tests computed LABEL and ROLE of all elements matching selector using existing
    verifyLabelsBySelector(), verifyRolesBySelector() functions and passes a test name prefix
    to ensure uniqueness.

  Ex: <div aria-label="foo" role="button"
        data-testname="div with role=button is labelled via aria-label"
        data-expectedlabel="foo"
        data-expectedrole="button"
        class="ex-role-and-label">

      AriaUtils.verifyRolesAndLabelsBySelector(".ex-role-and-label")

  */
  verifyRolesAndLabelsBySelector: function(selector) {
    let labelTestNamePrefix = "Label: ";
    let roleTestNamePrefix = "Role: ";
    const els = document.querySelectorAll(selector);
    if (!els.length) {
      throw `Selector passed in verifyRolesAndLabelsBySelector("${selector}") should match at least one element.`;
    }
    for (const el of els) {
      el.classList.add("ex-label-only");
      el.classList.add("ex-role-only");
    }
    this.verifyLabelsBySelector(".ex-label-only", labelTestNamePrefix);
    this.verifyRolesBySelector(".ex-role-only", roleTestNamePrefix);
  },
};

