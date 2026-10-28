// This file exposes the `formSubmissionTemplate` function, which can be used
// to create tests for the form submission encoding of various enctypes:
//
//   const urlencodedTest = formSubmissionTemplate(
//     "application/x-www-form-urlencoded",
//     (expected, _actualFormBody) => expected
//   );
//
//   urlencodedTest({
//     name: "a",
//     value: "b",
//     expected: "a=b",
//     formEncoding: "UTF-8", // optional
//     description: "Simple urlencoded test"
//   });
//
// The above call to `urlencodedTest` tests the urlencoded form submission for a
// form whose entry list contains a single entry with name "a" and value "b",
// and it checks that the form payload matches the `expected` property after
// isomorphic-encoding.
//
// Since per the spec no normalization of the form entries should happen before
// the actual form encoding, each call to `urlencodedTest` will in fact add two
// tests: one submitting the entry as a form control (marked "normal form"), and
// one adding the entry through the `formdata` event (marked "formdata event").
// Both cases are compared against the same expected value.
//
// Since multipart/form-data boundary strings can't be predicted ahead of time,
// the second parameter of `formSubmissionTemplate` allows transforming the
// expected value passed to each test. The second argument of that callback
// is the actual form body (isomorphic-decoded). When this callback is used, the
// `expected` property doesn't need to be a string.

(() => {
  // Using echo-content-escaped.py rather than
  // /fetch/api/resources/echo-content.py to work around WebKit not
  // percent-encoding \x00, which causes the response to be detected as
  // a binary file and served as a download.
  const ACTION_URL = "/FileAPI/file/resources/echo-content-escaped.py";

  const IFRAME_NAME = "formtargetframe";

  // Undoes the escapes from echo-content-escaped.py
  function unescape(str) {
    return str
      .replace(/\r\n?|\n/g, "\r\n")
      .replace(
        /\\x[0-9A-Fa-f]{2}/g,
        (escape) => String.fromCodePoint(parseInt(escape.substring(2), 16)),
      )
      .replace(/\\\\/g, "\\");
  }

  // Tests the form submission of an entry list containing a single entry.
  //
  // `expectedBuilder` is a function that takes in the actual form body
  // (necessary to get the multipart/form-data payload) and returns the form
  // body that should be expected.
  //
  // If `testFormData` is false, the form entry will be submitted in for
  // controls. If it is true, it will submitted by modifying the entry list
  // during the `formdata` event.
  async function formSubmissionTest({
    name,
    value,
    expectedBuilder,
    enctype,
    formEncoding,
    testFormData = false,
    testCase,
  }) {
    if (document.readyState !== "complete") {
      await new Promise((resolve) => addEventListener("load", resolve));
    }

    const formTargetFrame = Object.assign(document.createElement("iframe"), {
      name: IFRAME_NAME,
    });
    document.body.append(formTargetFrame);
    testCase.add_cleanup(() => {
      document.body.removeChild(formTargetFrame);
    });

    const form = Object.assign(document.createElement("form"), {
      acceptCharset: formEncoding,
      action: ACTION_URL,
      method: "POST",
      enctype,
      target: IFRAME_NAME,
    });
    document.body.append(form);
    testCase.add_cleanup(() => {
      document.body.removeChild(form);
    });

    if (!testFormData) {
      const input = document.createElement("input");
      input.name = name;
      if (value instanceof File) {
        input.type = "file";
        const dataTransfer = new DataTransfer();
        dataTransfer.items.add(value);
        input.files = dataTransfer.files;
      } else {
        input.type = "hidden";
        input.value = value;
      }
      form.append(input);
    } else {
      form.addEventListener("formdata", (evt) => {
        evt.formData.append(name, value);
      });
    }

    await new Promise((resolve) => {
      form.submit();
      formTargetFrame.onload = resolve;
    });

    const serialized = unescape(
      formTargetFrame.contentDocument.body.textContent,
    );
    const expected = expectedBuilder(serialized);
    assert_equals(serialized, expected);
  }

  // This function returns a function to add individual form tests corresponding
  // to some enctype.
  // `expectedBuilder` is an optional callback that takes two parameters:
  // `expected` (the `expected` property passed to a test) and `actualFormBody`
  // (the actual form body submitted by the browser, isomorphic-decoded). It
  // must return the correct form body that should have been submitted,
  // isomorphic-encoded. This is necessary in order to account for the
  // multipart/form-data boundary.
  //
  // The returned function takes an object with the following properties:
  // - `name`, the form entry's name. Must be a string.
  // - `value`, the form entry's value, either a string or a `File` object.
  // - `expected`, the expected form body. Usually a string, but it can be
  //   anything depending on `expectedBuilder`.
  // - `formEncoding` (optional), the character encoding used for submitting the
  //   form.
  // - `description`, used as part of the testharness test's description.
  window.formSubmissionTemplate = (
    enctype,
    expectedBuilder = (expected) => expected
  ) => {
    function form({
      name,
      value,
      expected,
      formEncoding = "utf-8",
      description,
    }) {
      const commonParams = {
        name,
        value,
        expectedBuilder: expectedBuilder.bind(null, expected),
        enctype,
        formEncoding,
      };

      // Normal form
      promise_test(
        (testCase) =>
          formSubmissionTest({
            ...commonParams,
            testCase,
          }),
        `${enctype}: ${description} (normal form)`,
      );

      // formdata event
      promise_test(
        (testCase) =>
          formSubmissionTest({
            ...commonParams,
            testFormData: true,
            testCase,
          }),
        `${enctype}: ${description} (formdata event)`,
      );
    }

    return form;
  };
})();
