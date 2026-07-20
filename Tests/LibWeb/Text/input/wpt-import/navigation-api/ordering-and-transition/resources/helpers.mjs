const variants = new Set((new URLSearchParams(location.search)).keys());

export function hasVariant(name) {
  return variants.has(name);
}

export class Recorder {
  #events = [];
  #errors = [];
  #navigationAPI;
  #domExceptionConstructor;
  #location;
  #skipCurrentChange;
  #finalExpectedEvent;
  #finalExpectedEventCount;
  #currentFinalEventCount = 0;

  #readyToAssertResolve;
  #readyToAssertPromise = new Promise(resolve => { this.#readyToAssertResolve = resolve; });

  constructor({ window = self, skipCurrentChange = false, finalExpectedEvent, finalExpectedEventCount = 1 }) {
    assert_equals(typeof finalExpectedEvent, "string", "Must pass a string for finalExpectedEvent");

    this.#navigationAPI = window.navigation;
    this.#domExceptionConstructor = window.DOMException;
    this.#location = window.location;

    this.#skipCurrentChange = skipCurrentChange;
    this.#finalExpectedEvent = finalExpectedEvent;
    this.#finalExpectedEventCount = finalExpectedEventCount;
  }

  setUpNavigationAPIListeners() {
    this.#navigationAPI.addEventListener("navigate", e => {
      this.record("navigate");

      e.signal.addEventListener("abort", () => {
        this.recordWithError("AbortSignal abort", e.signal.reason);
      });
    });

    this.#navigationAPI.addEventListener("navigateerror", e => {
      this.recordWithError("navigateerror", e.error);

      this.#navigationAPI.transition?.finished.then(
        () => this.record("transition.finished fulfilled"),
        err => this.recordWithError("transition.finished rejected", err)
      );
    });

    this.#navigationAPI.addEventListener("navigatesuccess", () => {
      this.record("navigatesuccess");

      this.#navigationAPI.transition?.finished.then(
        () => this.record("transition.finished fulfilled"),
        err => this.recordWithError("transition.finished rejected", err)
      );
    });

    if (!this.#skipCurrentChange) {
      this.#navigationAPI.addEventListener("currententrychange", () => this.record("currententrychange"));
    }
  }

  setUpResultListeners(result, suffix = "") {

    result.committed.then(
      () => this.record(`committed fulfilled${suffix}`),
      err => this.recordWithError(`committed rejected${suffix}`, err)
    );

    result.finished.then(
      () => this.record(`finished fulfilled${suffix}`),
      err => this.recordWithError(`finished rejected${suffix}`, err)
    );

    this.#navigationAPI.transition?.committed?.then(
      () => this.record(`transition.committed fulfilled${suffix}`),
      err => this.recordWithError(`transition.committed rejected${suffix}`, err)
    );
  }

  record(name) {
    const transitionProps = this.#navigationAPI.transition === null ? null : {
      from: this.#navigationAPI.transition.from,
      navigationType: this.#navigationAPI.transition.navigationType
    };

    this.#events.push({ name, location: this.#location.hash, transitionProps });

    if (name === this.#finalExpectedEvent && ++this.#currentFinalEventCount === this.#finalExpectedEventCount) {
      this.#readyToAssertResolve();
    }
  }

  recordWithError(name, errorObject) {
    this.record(name);
    this.#errors.push({ name, errorObject });
  }

  get readyToAssert() {
    return this.#readyToAssertPromise;
  }

  // Usage:
  //   recorder.assert([
  //     /* event name, location.hash value, navigation.transition properties */
  //     ["currententrychange", "", null],
  //     ["committed fulfilled", "#1", { from, navigationType }],
  //     ...
  //   ]);
  //
  // The array format is to avoid repitition at the call site, but I recommend
  // you document it like above.
  //
  // This will automatically also assert that any error objects recorded are
  // equal to each other. Use the other assert functions to check the actual
  // contents of the error objects.
  assert(expectedAsArray) {
    if (this.#skipCurrentChange) {
      expectedAsArray = expectedAsArray.filter(expected => expected[0] !== "currententrychange");
    }
    // TODO: Remove once https://github.com/whatwg/html/pull/10919 is merged.
    if (!('committed' in NavigationTransition.prototype)) {
      expectedAsArray = expectedAsArray.filter(expected => {
         return !expected[0].includes("transition.committed fulfilled") &&
                !expected[0].includes("transition.committed rejected")
      });
    }

    // Doing this up front gives nicer error messages because
    // assert_array_equals is nice.
    const recordedNames = this.#events.map(e => e.name);
    const expectedNames = expectedAsArray.map(e => e[0]);
    assert_array_equals(recordedNames, expectedNames);

    for (let i = 0; i < expectedAsArray.length; ++i) {
      const recorded = this.#events[i];
      const expected = expectedAsArray[i];

      assert_equals(
        recorded.location,
        expected[1],
        `event ${i} (${recorded.name}): location.hash value`
      );

      if (expected[2] === null) {
        assert_equals(
          recorded.transitionProps,
          null,
          `event ${i} (${recorded.name}): navigation.transition expected to be null`
        );
      } else {
        assert_not_equals(
          recorded.transitionProps,
          null,
          `event ${i} (${recorded.name}): navigation.transition expected not to be null`
        );
        assert_equals(
          recorded.transitionProps.from,
          expected[2].from,
          `event ${i} (${recorded.name}): navigation.transition.from`
        );
        assert_equals(
          recorded.transitionProps.navigationType,
          expected[2].navigationType,
          `event ${i} (${recorded.name}): navigation.transition.navigationType`
        );
      }
    }

    if (this.#errors.length > 1) {
      for (let i = 1; i < this.#errors.length; ++i) {
        assert_equals(
          this.#errors[i].errorObject,
          this.#errors[0].errorObject,
          `error objects must match: error object for ${this.#errors[i].name} did not match the one for ${this.#errors[0].name}`
        );
      }
    }
  }

  assertErrorsAreAbortErrors() {
    assert_greater_than(
      this.#errors.length,
      0,
      "No errors were recorded but assertErrorsAreAbortErrors() was called"
    );

    // Assume assert() has been called so all error objects are the same.
    const { errorObject } = this.#errors[0];
    assert_throws_dom("AbortError", this.#domExceptionConstructor, () => { throw errorObject; });
  }

  assertErrorsAre(expectedErrorObject) {
    assert_greater_than(
      this.#errors.length,
      0,
      "No errors were recorded but assertErrorsAre() was called"
    );

    // Assume assert() has been called so all error objects are the same.
    const { errorObject } = this.#errors[0];
    assert_equals(errorObject, expectedErrorObject);
  }
}
