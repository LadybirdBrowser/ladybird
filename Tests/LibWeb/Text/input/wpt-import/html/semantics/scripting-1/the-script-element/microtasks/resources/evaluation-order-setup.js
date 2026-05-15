globalThis.setup({allow_uncaught_exception: true});

// Must be called after previous tests are completed.
globalThis.setupTest = (description, expectedLog) => {
  globalThis.log = [];
  globalThis.onerror = message => {
      globalThis.log.push("global-error");
      return true;
  };
  globalThis.onunhandledrejection =
      () => globalThis.log.push('unhandled-promise-rejection');

  globalThis.unreachable = () => globalThis.log.push("unreachable");

  globalThis.test_load = async_test(description);
  globalThis.testDone = globalThis.test_load.step_func_done(() => {
    assert_array_equals(globalThis.log, expectedLog);
  });

  if (!('Window' in globalThis && globalThis instanceof Window)) {
    // In workers, there are no <script> load event, so scheduling `testDone()`
    // here, assuming the target script is loaded and evaluated soon.
    globalThis.test_load.step_timeout(() => globalThis.testDone(), 1000);

    // In workers, call `done()` here because the auto-generated `done()` calls
    // by `any.js` etc. are at the end of main script and thus are not
    // evaluated when the target script throws an exception.
    done();
  }
};
