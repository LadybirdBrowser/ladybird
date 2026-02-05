// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// See https://github.com/web-platform-tests/wpt/issues/12781 for information on
// the purpose of audit.js, and why testharness.js does not suffice.

/**
 * @fileOverview  WebAudio layout test utility library. Built around W3C's
 *                testharness.js. Includes asynchronous test task manager,
 *                assertion utilities.
 * @dependency    testharness.js
 */


(function() {

  'use strict';

  // Selected methods from testharness.js.
  let testharnessProperties = [
    'test', 'async_test', 'promise_test', 'promise_rejects_js', 'generate_tests',
    'setup', 'done', 'assert_true', 'assert_false'
  ];

  // Check if testharness.js is properly loaded. Throw otherwise.
  for (let name in testharnessProperties) {
    if (!self.hasOwnProperty(testharnessProperties[name]))
      throw new Error('Cannot proceed. testharness.js is not loaded.');
  }
})();


window.Audit = (function() {

  'use strict';

  // NOTE: Moving this method (or any other code above) will change the location
  // of 'CONSOLE ERROR...' message in the expected text files.
  function _logError(message) {
    console.error('[audit.js] ' + message);
  }

  function _logPassed(message) {
    test(function(arg) {
      assert_true(true);
    }, message);
  }

  function _logFailed(message, detail) {
    test(function() {
      assert_true(false, detail);
    }, message);
  }

  function _throwException(message) {
    throw new Error(message);
  }

  // TODO(hongchan): remove this hack after confirming all the tests are
  // finished correctly. (crbug.com/708817)
  const _testharnessDone = window.done;
  window.done = () => {
    _throwException('Do NOT call done() method from the test code.');
  };

  // Generate a descriptive string from a target value in various types.
  function _generateDescription(target, options) {
    let targetString;

    switch (typeof target) {
      case 'object':
        // Handle Arrays.
        if (target instanceof Array || target instanceof Float32Array ||
            target instanceof Float64Array || target instanceof Uint8Array) {
          let arrayElements = target.length < options.numberOfArrayElements ?
              String(target) :
              String(target.slice(0, options.numberOfArrayElements)) + '...';
          targetString = '[' + arrayElements + ']';
        } else if (target === null) {
          targetString = String(target);
        } else {
          targetString = '' + String(target).split(/[\s\]]/)[1];
        }
        break;
      case 'function':
        if (Error.isPrototypeOf(target)) {
          targetString = "EcmaScript error " + target.name;
        } else {
          targetString = String(target);
        }
        break;
      default:
        targetString = String(target);
        break;
    }

    return targetString;
  }

  // Return a string suitable for printing one failed element in
  // |beCloseToArray|.
  function _formatFailureEntry(index, actual, expected, abserr, threshold) {
    return '\t[' + index + ']\t' + actual.toExponential(16) + '\t' +
        expected.toExponential(16) + '\t' + abserr.toExponential(16) + '\t' +
        (abserr / Math.abs(expected)).toExponential(16) + '\t' +
        threshold.toExponential(16);
  }

  // Compute the error threshold criterion for |beCloseToArray|
  function _closeToThreshold(abserr, relerr, expected) {
    return Math.max(abserr, relerr * Math.abs(expected));
  }

  /**
   * @class Should
   * @description Assertion subtask for the Audit task.
   * @param {Task} parentTask           Associated Task object.
   * @param {Any} actual                Target value to be tested.
   * @param {String} actualDescription  String description of the test target.
   */
  class Should {
    constructor(parentTask, actual, actualDescription) {
      this._task = parentTask;

      this._actual = actual;
      this._actualDescription = (actualDescription || null);
      this._expected = null;
      this._expectedDescription = null;

      this._detail = '';
      // If true and the test failed, print the actual value at the
      // end of the message.
      this._printActualForFailure = true;

      this._result = null;

      /**
       * @param {Number} numberOfErrors   Number of errors to be printed.
       * @param {Number} numberOfArrayElements  Number of array elements to be
       *                                        printed in the test log.
       * @param {Boolean} verbose         Verbose output from the assertion.
       */
      this._options = {
        numberOfErrors: 4,
        numberOfArrayElements: 16,
        verbose: false
      };
    }

    _processArguments(args) {
      if (args.length === 0)
        return;

      if (args.length > 0)
        this._expected = args[0];

      if (typeof args[1] === 'string') {
        // case 1: (expected, description, options)
        this._expectedDescription = args[1];
        Object.assign(this._options, args[2]);
      } else if (typeof args[1] === 'object') {
        // case 2: (expected, options)
        Object.assign(this._options, args[1]);
      }
    }

    _buildResultText() {
      if (this._result === null)
        _throwException('Illegal invocation: the assertion is not finished.');

      let actualString = _generateDescription(this._actual, this._options);

      // Use generated text when the description is not provided.
      if (!this._actualDescription)
        this._actualDescription = actualString;

      if (!this._expectedDescription) {
        this._expectedDescription =
            _generateDescription(this._expected, this._options);
      }

      // For the assertion with a single operand.
      this._detail =
          this._detail.replace(/\$\{actual\}/g, this._actualDescription);

      // If there is a second operand (i.e. expected value), we have to build
      // the string for it as well.
      this._detail =
          this._detail.replace(/\$\{expected\}/g, this._expectedDescription);

      // If there is any property in |_options|, replace the property name
      // with the value.
      for (let name in this._options) {
        if (name === 'numberOfErrors' || name === 'numberOfArrayElements' ||
            name === 'verbose') {
          continue;
        }

        // The RegExp key string contains special character. Take care of it.
        let re = '\$\{' + name + '\}';
        re = re.replace(/([.*+?^=!:${}()|\[\]\/\\])/g, '\\$1');
        this._detail = this._detail.replace(
            new RegExp(re, 'g'), _generateDescription(this._options[name]));
      }

      // If the test failed, add the actual value at the end.
      if (this._result === false && this._printActualForFailure === true) {
        this._detail += ' Got ' + actualString + '.';
      }
    }

    _finalize() {
      if (this._result) {
        _logPassed('  ' + this._detail);
      } else {
        _logFailed('X ' + this._detail);
      }

      // This assertion is finished, so update the parent task accordingly.
      this._task.update(this);

      // TODO(hongchan): configurable 'detail' message.
    }

    _assert(condition, passDetail, failDetail) {
      this._result = Boolean(condition);
      this._detail = this._result ? passDetail : failDetail;
      this._buildResultText();
      this._finalize();

      return this._result;
    }

    get result() {
      return this._result;
    }

    get detail() {
      return this._detail;
    }

    /**
     * should() assertions.
     *
     * @example All the assertions can have 1, 2 or 3 arguments:
     *   should().doAssert(expected);
     *   should().doAssert(expected, options);
     *   should().doAssert(expected, expectedDescription, options);
     *
     * @param {Any} expected                  Expected value of the assertion.
     * @param {String} expectedDescription    Description of expected value.
     * @param {Object} options                Options for assertion.
     * @param {Number} options.numberOfErrors Number of errors to be printed.
     *                                        (if applicable)
     * @param {Number} options.numberOfArrayElements  Number of array elements
     *                                                to be printed. (if
     *                                                applicable)
     * @notes Some assertions can have additional options for their specific
     *        testing.
     */

    /**
     * Check if |actual| exists.
     *
     * @example
     *   should({}, 'An empty object').exist();
     * @result
     *   "PASS   An empty object does exist."
     */
    exist() {
      return this._assert(
          this._actual !== null && this._actual !== undefined,
          '${actual} does exist.', '${actual} does not exist.');
    }

    /**
     * Check if |actual| operation wrapped in a function throws an exception
     * with a expected error type correctly. |expected| is optional. If it is an
     * instance of DOMException, then the description (second argument) can be
     * provided to be more strict about the expected exception type. |expected|
     * also can be other generic error types such as TypeError, RangeError or
     * etc.
     *
     * @example
     *   should(() => { let a = b; }, 'A bad code').throw();
     *   should(() => { new SomeConstructor(); }, 'A bad construction')
     *       .throw(DOMException, 'NotSupportedError');
     *   should(() => { let c = d; }, 'Assigning d to c')
     *       .throw(ReferenceError);
     *   should(() => { let e = f; }, 'Assigning e to f')
     *       .throw(ReferenceError, { omitErrorMessage: true });
     *
     * @result
     *   "PASS   A bad code threw an exception of ReferenceError: b is not
     *       defined."
     *   "PASS   A bad construction threw DOMException:NotSupportedError."
     *   "PASS   Assigning d to c threw ReferenceError: d is not defined."
     *   "PASS   Assigning e to f threw ReferenceError: [error message
     *       omitted]."
     */
    throw() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let didThrowCorrectly = false;
      let passDetail, failDetail;

      try {
        // This should throw.
        this._actual();
        // Catch did not happen, so the test is failed.
        failDetail = '${actual} did not throw an exception.';
      } catch (error) {
        let errorMessage = this._options.omitErrorMessage ?
            ': [error message omitted]' :
            ': "' + error.message + '"';
        if (this._expected === null || this._expected === undefined) {
          // The expected error type was not given.
          didThrowCorrectly = true;
          passDetail = '${actual} threw ' + error.name + errorMessage + '.';
        } else if (this._expected === DOMException &&
                   this._expectedDescription !== undefined) {
          // Handles DOMException with an expected exception name.
          if (this._expectedDescription === error.name) {
            didThrowCorrectly = true;
            passDetail = '${actual} threw ${expected}' + errorMessage + '.';
          } else {
            didThrowCorrectly = false;
            failDetail =
                '${actual} threw "' + error.name + '" instead of ${expected}.';
          }
        } else if (this._expected == error.constructor) {
          // Handler other error types.
          didThrowCorrectly = true;
          passDetail = '${actual} threw ' + error.name + errorMessage + '.';
        } else {
          didThrowCorrectly = false;
          failDetail =
              '${actual} threw "' + error.name + '" instead of ${expected}.';
        }
      }

      return this._assert(didThrowCorrectly, passDetail, failDetail);
    }

    /**
     * Check if |actual| operation wrapped in a function does not throws an
     * exception correctly.
     *
     * @example
     *   should(() => { let foo = 'bar'; }, 'let foo = "bar"').notThrow();
     *
     * @result
     *   "PASS   let foo = "bar" did not throw an exception."
     */
    notThrow() {
      this._printActualForFailure = false;

      let didThrowCorrectly = false;
      let passDetail, failDetail;

      try {
        this._actual();
        passDetail = '${actual} did not throw an exception.';
      } catch (error) {
        didThrowCorrectly = true;
        failDetail = '${actual} incorrectly threw ' + error.name + ': "' +
            error.message + '".';
      }

      return this._assert(!didThrowCorrectly, passDetail, failDetail);
    }

    /**
     * Check if |actual| promise is resolved correctly. Note that the returned
     * result from promise object will be passed to the following then()
     * function.
     *
     * @example
     *   should('My promise', promise).beResolve().then((result) => {
     *     log(result);
     *   });
     *
     * @result
     *   "PASS   My promise resolved correctly."
     *   "FAIL X My promise rejected *INCORRECTLY* with _ERROR_."
     */
    beResolved() {
      return this._actual.then(
          function(result) {
            this._assert(true, '${actual} resolved correctly.', null);
            return result;
          }.bind(this),
          function(error) {
            this._assert(
                false, null,
                '${actual} rejected incorrectly with ' + error + '.');
          }.bind(this));
    }

    /**
     * Check if |actual| promise is rejected correctly.
     *
     * @example
     *   should('My promise', promise).beRejected().then(nextStuff);
     *
     * @result
     *   "PASS   My promise rejected correctly (with _ERROR_)."
     *   "FAIL X My promise resolved *INCORRECTLY*."
     */
    beRejected() {
      return this._actual.then(
          function() {
            this._assert(false, null, '${actual} resolved incorrectly.');
          }.bind(this),
          function(error) {
            this._assert(
                true, '${actual} rejected correctly with ' + error + '.', null);
          }.bind(this));
    }

    /**
     * Check if |actual| promise is rejected correctly.
     *
     * @example
     *   should(promise, 'My promise').beRejectedWith('_ERROR_').then();
     *
     * @result
     *   "PASS   My promise rejected correctly with _ERROR_."
     *   "FAIL X My promise rejected correctly but got _ACTUAL_ERROR instead of
     *           _EXPECTED_ERROR_."
     *   "FAIL X My promise resolved incorrectly."
     */
    beRejectedWith() {
      this._processArguments(arguments);

      return this._actual.then(
          function() {
            this._assert(false, null, '${actual} resolved incorrectly.');
          }.bind(this),
          function(error) {
            if (this._expected !== error.name) {
              this._assert(
                  false, null,
                  '${actual} rejected correctly but got ' + error.name +
                      ' instead of ' + this._expected + '.');
            } else {
              this._assert(
                  true,
                  '${actual} rejected correctly with ' + this._expected + '.',
                  null);
            }
          }.bind(this));
    }

    /**
     * Check if |actual| is a boolean true.
     *
     * @example
     *   should(3 < 5, '3 < 5').beTrue();
     *
     * @result
     *   "PASS   3 < 5 is true."
     */
    beTrue() {
      return this._assert(
          this._actual === true, '${actual} is true.',
          '${actual} is not true.');
    }

    /**
     * Check if |actual| is a boolean false.
     *
     * @example
     *   should(3 > 5, '3 > 5').beFalse();
     *
     * @result
     *   "PASS   3 > 5 is false."
     */
    beFalse() {
      return this._assert(
          this._actual === false, '${actual} is false.',
          '${actual} is not false.');
    }

    /**
     * Check if |actual| is strictly equal to |expected|. (no type coercion)
     *
     * @example
     *   should(1).beEqualTo(1);
     *
     * @result
     *   "PASS   1 is equal to 1."
     */
    beEqualTo() {
      this._processArguments(arguments);
      return this._assert(
          this._actual === this._expected, '${actual} is equal to ${expected}.',
          '${actual} is not equal to ${expected}.');
    }

    /**
     * Check if |actual| is not equal to |expected|.
     *
     * @example
     *   should(1).notBeEqualTo(2);
     *
     * @result
     *   "PASS   1 is not equal to 2."
     */
    notBeEqualTo() {
      this._processArguments(arguments);
      return this._assert(
          this._actual !== this._expected,
          '${actual} is not equal to ${expected}.',
          '${actual} should not be equal to ${expected}.');
    }

    /**
     * check if |actual| is NaN
     *
     * @example
     *   should(NaN).beNaN();
     *
     * @result
     *   "PASS   NaN is NaN"
     *
     */
    beNaN() {
      this._processArguments(arguments);
      return this._assert(
          isNaN(this._actual),
          '${actual} is NaN.',
          '${actual} is not NaN but should be.');
    }

    /**
     * check if |actual| is NOT NaN
     *
     * @example
     *   should(42).notBeNaN();
     *
     * @result
     *   "PASS   42 is not NaN"
     *
     */
    notBeNaN() {
      this._processArguments(arguments);
      return this._assert(
          !isNaN(this._actual),
          '${actual} is not NaN.',
          '${actual} is NaN but should not be.');
    }

    /**
     * Check if |actual| is greater than |expected|.
     *
     * @example
     *   should(2).beGreaterThanOrEqualTo(2);
     *
     * @result
     *   "PASS   2 is greater than or equal to 2."
     */
    beGreaterThan() {
      this._processArguments(arguments);
      return this._assert(
          this._actual > this._expected,
          '${actual} is greater than ${expected}.',
          '${actual} is not greater than ${expected}.');
    }

    /**
     * Check if |actual| is greater than or equal to |expected|.
     *
     * @example
     *   should(2).beGreaterThan(1);
     *
     * @result
     *   "PASS   2 is greater than 1."
     */
    beGreaterThanOrEqualTo() {
      this._processArguments(arguments);
      return this._assert(
          this._actual >= this._expected,
          '${actual} is greater than or equal to ${expected}.',
          '${actual} is not greater than or equal to ${expected}.');
    }

    /**
     * Check if |actual| is less than |expected|.
     *
     * @example
     *   should(1).beLessThan(2);
     *
     * @result
     *   "PASS   1 is less than 2."
     */
    beLessThan() {
      this._processArguments(arguments);
      return this._assert(
          this._actual < this._expected, '${actual} is less than ${expected}.',
          '${actual} is not less than ${expected}.');
    }

    /**
     * Check if |actual| is less than or equal to |expected|.
     *
     * @example
     *   should(1).beLessThanOrEqualTo(1);
     *
     * @result
     *   "PASS   1 is less than or equal to 1."
     */
    beLessThanOrEqualTo() {
      this._processArguments(arguments);
      return this._assert(
          this._actual <= this._expected,
          '${actual} is less than or equal to ${expected}.',
          '${actual} is not less than or equal to ${expected}.');
    }

    /**
     * Check if |actual| array is filled with a constant |expected| value.
     *
     * @example
     *   should([1, 1, 1]).beConstantValueOf(1);
     *
     * @result
     *   "PASS   [1,1,1] contains only the constant 1."
     */
    beConstantValueOf() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let passDetail, failDetail;
      let errors = {};

      let actual = this._actual;
      let expected = this._expected;
      for (let index = 0; index < actual.length; ++index) {
        if (actual[index] !== expected)
          errors[index] = actual[index];
      }

      let numberOfErrors = Object.keys(errors).length;
      passed = numberOfErrors === 0;

      if (passed) {
        passDetail = '${actual} contains only the constant ${expected}.';
      } else {
        let counter = 0;
        failDetail =
            '${actual}: Expected ${expected} for all values but found ' +
            numberOfErrors + ' unexpected values: ';
        failDetail += '\n\tIndex\tActual';
        for (let errorIndex in errors) {
          failDetail += '\n\t[' + errorIndex + ']' +
              '\t' + errors[errorIndex];
          if (++counter >= this._options.numberOfErrors) {
            failDetail +=
                '\n\t...and ' + (numberOfErrors - counter) + ' more errors.';
            break;
          }
        }
      }

      return this._assert(passed, passDetail, failDetail);
    }

    /**
     * Check if |actual| array is not filled with a constant |expected| value.
     *
     * @example
     *   should([1, 0, 1]).notBeConstantValueOf(1);
     *   should([0, 0, 0]).notBeConstantValueOf(0);
     *
     * @result
     *   "PASS   [1,0,1] is not constantly 1 (contains 1 different value)."
     *   "FAIL X [0,0,0] should have contain at least one value different
     *     from 0."
     */
    notBeConstantValueOf() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let passDetail;
      let failDetail;
      let differences = {};

      let actual = this._actual;
      let expected = this._expected;
      for (let index = 0; index < actual.length; ++index) {
        if (actual[index] !== expected)
          differences[index] = actual[index];
      }

      let numberOfDifferences = Object.keys(differences).length;
      passed = numberOfDifferences > 0;

      if (passed) {
        let valueString = numberOfDifferences > 1 ? 'values' : 'value';
        passDetail = '${actual} is not constantly ${expected} (contains ' +
            numberOfDifferences + ' different ' + valueString + ').';
      } else {
        failDetail = '${actual} should have contain at least one value ' +
            'different from ${expected}.';
      }

      return this._assert(passed, passDetail, failDetail);
    }

    /**
     * Check if |actual| array is identical to |expected| array element-wise.
     *
     * @example
     *   should([1, 2, 3]).beEqualToArray([1, 2, 3]);
     *
     * @result
     *   "[1,2,3] is identical to the array [1,2,3]."
     */
    beEqualToArray() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let passDetail, failDetail;
      let errorIndices = [];

      if (this._actual.length !== this._expected.length) {
        passed = false;
        failDetail = 'The array length does not match.';
        return this._assert(passed, passDetail, failDetail);
      }

      let actual = this._actual;
      let expected = this._expected;
      for (let index = 0; index < actual.length; ++index) {
        if (actual[index] !== expected[index])
          errorIndices.push(index);
      }

      passed = errorIndices.length === 0;

      if (passed) {
        passDetail = '${actual} is identical to the array ${expected}.';
      } else {
        let counter = 0;
        failDetail =
            '${actual} expected to be equal to the array ${expected} ' +
            'but differs in ' + errorIndices.length + ' places:' +
            '\n\tIndex\tActual\t\t\tExpected';
        for (let index of errorIndices) {
          failDetail += '\n\t[' + index + ']' +
              '\t' + this._actual[index].toExponential(16) + '\t' +
              this._expected[index].toExponential(16);
          if (++counter >= this._options.numberOfErrors) {
            failDetail += '\n\t...and ' + (errorIndices.length - counter) +
                ' more errors.';
            break;
          }
        }
      }

      return this._assert(passed, passDetail, failDetail);
    }

    /**
     * Check if |actual| array contains only the values in |expected| in the
     * order of values in |expected|.
     *
     * @example
     *   Should([1, 1, 3, 3, 2], 'My random array').containValues([1, 3, 2]);
     *
     * @result
     *   "PASS   [1,1,3,3,2] contains all the expected values in the correct
     *           order: [1,3,2].
     */
    containValues() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let indexedActual = [];
      let firstErrorIndex = null;

      // Collect the unique value sequence from the actual.
      for (let i = 0, prev = null; i < this._actual.length; i++) {
        if (this._actual[i] !== prev) {
          indexedActual.push({index: i, value: this._actual[i]});
          prev = this._actual[i];
        }
      }

      // Compare against the expected sequence.
      let failMessage =
          '${actual} expected to have the value sequence of ${expected} but ' +
          'got ';
      if (this._expected.length === indexedActual.length) {
        for (let j = 0; j < this._expected.length; j++) {
          if (this._expected[j] !== indexedActual[j].value) {
            firstErrorIndex = indexedActual[j].index;
            passed = false;
            failMessage += this._actual[firstErrorIndex] + ' at index ' +
                firstErrorIndex + '.';
            break;
          }
        }
      } else {
        passed = false;
        let indexedValues = indexedActual.map(x => x.value);
        failMessage += `${indexedActual.length} values, [${
            indexedValues}], instead of ${this._expected.length}.`;
      }

      return this._assert(
          passed,
          '${actual} contains all the expected values in the correct order: ' +
              '${expected}.',
          failMessage);
    }

    /**
     * Check if |actual| array does not have any glitches. Note that |threshold|
     * is not optional and is to define the desired threshold value.
     *
     * @example
     *   should([0.5, 0.5, 0.55, 0.5, 0.45, 0.5]).notGlitch(0.06);
     *
     * @result
     *   "PASS   [0.5,0.5,0.55,0.5,0.45,0.5] has no glitch above the threshold
     *           of 0.06."
     *
     */
    notGlitch() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let passDetail, failDetail;

      let actual = this._actual;
      let expected = this._expected;
      for (let index = 0; index < actual.length; ++index) {
        let diff = Math.abs(actual[index - 1] - actual[index]);
        if (diff >= expected) {
          passed = false;
          failDetail = '${actual} has a glitch at index ' + index +
              ' of size ' + diff + '.';
        }
      }

      passDetail =
          '${actual} has no glitch above the threshold of ${expected}.';

      return this._assert(passed, passDetail, failDetail);
    }

    /**
     * Check if |actual| is close to |expected| using the given relative error
     * |threshold|.
     *
     * @example
     *   should(2.3).beCloseTo(2, { threshold: 0.3 });
     *
     * @result
     *   "PASS    2.3 is 2 within an error of 0.3."
     * @param {Object} options              Options for assertion.
     * @param {Number} options.threshold    Threshold value for the comparison.
     */
    beCloseTo() {
      this._processArguments(arguments);

      // The threshold is relative except when |expected| is zero, in which case
      // it is absolute.
      let absExpected = this._expected ? Math.abs(this._expected) : 1;
      let error = Math.abs(this._actual - this._expected) / absExpected;

      return this._assert(
          error <= this._options.threshold,
          '${actual} is ${expected} within an error of ${threshold}.',
          '${actual} is not close to ${expected} within a relative error of ' +
              '${threshold} (RelErr=' + error + ').');
    }

    /**
     * Check if |target| array is close to |expected| array element-wise within
     * a certain error bound given by the |options|.
     *
     * The error criterion is:
     *   abs(actual[k] - expected[k]) < max(absErr, relErr * abs(expected))
     *
     * If nothing is given for |options|, then absErr = relErr = 0. If
     * absErr = 0, then the error criterion is a relative error. A non-zero
     * absErr value produces a mix intended to handle the case where the
     * expected value is 0, allowing the target value to differ by absErr from
     * the expected.
     *
     * @param {Number} options.absoluteThreshold    Absolute threshold.
     * @param {Number} options.relativeThreshold    Relative threshold.
     */
    beCloseToArray() {
      this._processArguments(arguments);
      this._printActualForFailure = false;

      let passed = true;
      let passDetail, failDetail;

      // Parsing options.
      let absErrorThreshold = (this._options.absoluteThreshold || 0);
      let relErrorThreshold = (this._options.relativeThreshold || 0);

      // A collection of all of the values that satisfy the error criterion.
      // This holds the absolute difference between the target element and the
      // expected element.
      let errors = {};

      // Keep track of the max absolute error found.
      let maxAbsError = -Infinity, maxAbsErrorIndex = -1;

      // Keep track of the max relative error found, ignoring cases where the
      // relative error is Infinity because the expected value is 0.
      let maxRelError = -Infinity, maxRelErrorIndex = -1;

      let actual = this._actual;
      let expected = this._expected;

      for (let index = 0; index < expected.length; ++index) {
        let diff = Math.abs(actual[index] - expected[index]);
        let absExpected = Math.abs(expected[index]);
        let relError = diff / absExpected;

        if (diff >
            Math.max(absErrorThreshold, relErrorThreshold * absExpected)) {
          if (diff > maxAbsError) {
            maxAbsErrorIndex = index;
            maxAbsError = diff;
          }

          if (!isNaN(relError) && relError > maxRelError) {
            maxRelErrorIndex = index;
            maxRelError = relError;
          }

          errors[index] = diff;
        }
      }

      let numberOfErrors = Object.keys(errors).length;
      let maxAllowedErrorDetail = JSON.stringify({
        absoluteThreshold: absErrorThreshold,
        relativeThreshold: relErrorThreshold
      });

      if (numberOfErrors === 0) {
        // The assertion was successful.
        passDetail = '${actual} equals ${expected} with an element-wise ' +
            'tolerance of ' + maxAllowedErrorDetail + '.';
      } else {
        // Failed. Prepare the detailed failure log.
        passed = false;
        failDetail = '${actual} does not equal ${expected} with an ' +
            'element-wise tolerance of ' + maxAllowedErrorDetail + '.\n';

        // Print out actual, expected, absolute error, and relative error.
        let counter = 0;
        failDetail += '\tIndex\tActual\t\t\tExpected\t\tAbsError' +
            '\t\tRelError\t\tTest threshold';
        let printedIndices = [];
        for (let index in errors) {
          failDetail +=
              '\n' +
              _formatFailureEntry(
                  index, actual[index], expected[index], errors[index],
                  _closeToThreshold(
                      absErrorThreshold, relErrorThreshold, expected[index]));

          printedIndices.push(index);
          if (++counter > this._options.numberOfErrors) {
            failDetail +=
                '\n\t...and ' + (numberOfErrors - counter) + ' more errors.';
            break;
          }
        }

        // Finalize the error log: print out the location of both the maxAbs
        // error and the maxRel error so we can adjust thresholds appropriately
        // in the test.
        failDetail += '\n' +
            '\tMax AbsError of ' + maxAbsError.toExponential(16) +
            ' at index of ' + maxAbsErrorIndex + '.\n';
        if (printedIndices.find(element => {
              return element == maxAbsErrorIndex;
            }) === undefined) {
          // Print an entry for this index if we haven't already.
          failDetail +=
              _formatFailureEntry(
                  maxAbsErrorIndex, actual[maxAbsErrorIndex],
                  expected[maxAbsErrorIndex], errors[maxAbsErrorIndex],
                  _closeToThreshold(
                      absErrorThreshold, relErrorThreshold,
                      expected[maxAbsErrorIndex])) +
              '\n';
        }
        failDetail += '\tMax RelError of ' + maxRelError.toExponential(16) +
            ' at index of ' + maxRelErrorIndex + '.\n';
        if (printedIndices.find(element => {
              return element == maxRelErrorIndex;
            }) === undefined) {
          // Print an entry for this index if we haven't already.
          failDetail +=
              _formatFailureEntry(
                  maxRelErrorIndex, actual[maxRelErrorIndex],
                  expected[maxRelErrorIndex], errors[maxRelErrorIndex],
                  _closeToThreshold(
                      absErrorThreshold, relErrorThreshold,
                      expected[maxRelErrorIndex])) +
              '\n';
        }
      }

      return this._assert(passed, passDetail, failDetail);
    }

    /**
     * A temporary escape hat for printing an in-task message. The description
     * for the |actual| is required to get the message printed properly.
     *
     * TODO(hongchan): remove this method when the transition from the old Audit
     * to the new Audit is completed.
     * @example
     *   should(true, 'The message is').message('truthful!', 'false!');
     *
     * @result
     *   "PASS   The message is truthful!"
     */
    message(passDetail, failDetail) {
      return this._assert(
          this._actual, '${actual} ' + passDetail, '${actual} ' + failDetail);
    }

    /**
     * Check if |expected| property is truly owned by |actual| object.
     *
     * @example
     *   should(BaseAudioContext.prototype,
     *          'BaseAudioContext.prototype').haveOwnProperty('createGain');
     *
     * @result
     *   "PASS   BaseAudioContext.prototype has an own property of
     *       'createGain'."
     */
    haveOwnProperty() {
      this._processArguments(arguments);

      return this._assert(
          this._actual.hasOwnProperty(this._expected),
          '${actual} has an own property of "${expected}".',
          '${actual} does not own the property of "${expected}".');
    }


    /**
     * Check if |expected| property is not owned by |actual| object.
     *
     * @example
     *   should(BaseAudioContext.prototype,
     *          'BaseAudioContext.prototype')
     *       .notHaveOwnProperty('startRendering');
     *
     * @result
     *   "PASS   BaseAudioContext.prototype does not have an own property of
     *       'startRendering'."
     */
    notHaveOwnProperty() {
      this._processArguments(arguments);

      return this._assert(
          !this._actual.hasOwnProperty(this._expected),
          '${actual} does not have an own property of "${expected}".',
          '${actual} has an own the property of "${expected}".')
    }


    /**
     * Check if an object is inherited from a class. This looks up the entire
     * prototype chain of a given object and tries to find a match.
     *
     * @example
     *   should(sourceNode, 'A buffer source node')
     *       .inheritFrom('AudioScheduledSourceNode');
     *
     * @result
     *   "PASS   A buffer source node inherits from 'AudioScheduledSourceNode'."
     */
    inheritFrom() {
      this._processArguments(arguments);

      let prototypes = [];
      let currentPrototype = Object.getPrototypeOf(this._actual);
      while (currentPrototype) {
        prototypes.push(currentPrototype.constructor.name);
        currentPrototype = Object.getPrototypeOf(currentPrototype);
      }

      return this._assert(
          prototypes.includes(this._expected),
          '${actual} inherits from "${expected}".',
          '${actual} does not inherit from "${expected}".');
    }
  }


  // Task Class state enum.
  const TaskState = {PENDING: 0, STARTED: 1, FINISHED: 2};


  /**
   * @class Task
   * @description WebAudio testing task. Managed by TaskRunner.
   */
  class Task {
    /**
     * Task constructor.
     * @param  {Object} taskRunner Reference of associated task runner.
     * @param  {String||Object} taskLabel Task label if a string is given. This
     *                                    parameter can be a dictionary with the
     *                                    following fields.
     * @param  {String} taskLabel.label Task label.
     * @param  {String} taskLabel.description Description of task.
     * @param  {Function} taskFunction Task function to be performed.
     * @return {Object} Task object.
     */
    constructor(taskRunner, taskLabel, taskFunction) {
      this._taskRunner = taskRunner;
      this._taskFunction = taskFunction;

      if (typeof taskLabel === 'string') {
        this._label = taskLabel;
        this._description = null;
      } else if (typeof taskLabel === 'object') {
        if (typeof taskLabel.label !== 'string') {
          _throwException('Task.constructor:: task label must be string.');
        }
        this._label = taskLabel.label;
        this._description = (typeof taskLabel.description === 'string') ?
            taskLabel.description :
            null;
      } else {
        _throwException(
            'Task.constructor:: task label must be a string or ' +
            'a dictionary.');
      }

      this._state = TaskState.PENDING;
      this._result = true;

      this._totalAssertions = 0;
      this._failedAssertions = 0;
    }

    get label() {
      return this._label;
    }

    get state() {
      return this._state;
    }

    get result() {
      return this._result;
    }

    // Start the assertion chain.
    should(actual, actualDescription) {
      // If no argument is given, we cannot proceed. Halt.
      if (arguments.length === 0)
        _throwException('Task.should:: requires at least 1 argument.');

      return new Should(this, actual, actualDescription);
    }

    // Run this task. |this| task will be passed into the user-supplied test
    // task function.
    run(harnessTest) {
      this._state = TaskState.STARTED;
      this._harnessTest = harnessTest;
      // Print out the task entry with label and description.
      _logPassed(
          '> [' + this._label + '] ' +
          (this._description ? this._description : ''));

      return new Promise((resolve, reject) => {
        this._resolve = resolve;
        this._reject = reject;
        let result = this._taskFunction(this, this.should.bind(this));
        if (result && typeof result.then === "function") {
          result.then(() => this.done()).catch(reject);
        }
      });
    }

    // Update the task success based on the individual assertion/test inside.
    update(subTask) {
      // After one of tests fails within a task, the result is irreversible.
      if (subTask.result === false) {
        this._result = false;
        this._failedAssertions++;
      }

      this._totalAssertions++;
    }

    // Finish the current task and start the next one if available.
    done() {
      assert_equals(this._state, TaskState.STARTED)
      this._state = TaskState.FINISHED;

      let message = '< [' + this._label + '] ';

      if (this._result) {
        message += 'All assertions passed. (total ' + this._totalAssertions +
            ' assertions)';
        _logPassed(message);
      } else {
        message += this._failedAssertions + ' out of ' + this._totalAssertions +
            ' assertions were failed.'
        _logFailed(message);
      }

      this._resolve();
    }

    // Runs |subTask| |time| milliseconds later. |setTimeout| is not allowed in
    // WPT linter, so a thin wrapper around the harness's |step_timeout| is
    // used here.  Returns a Promise which is resolved after |subTask| runs.
    timeout(subTask, time) {
      return new Promise(resolve => {
        this._harnessTest.step_timeout(() => {
          let result = subTask();
          if (result && typeof result.then === "function") {
            // Chain rejection directly to the harness test Promise, to report
            // the rejection against the subtest even when the caller of
            // timeout does not handle the rejection.
            result.then(resolve, this._reject());
          } else {
            resolve();
          }
        }, time);
      });
    }

    isPassed() {
      return this._state === TaskState.FINISHED && this._result;
    }

    toString() {
      return '"' + this._label + '": ' + this._description;
    }
  }


  /**
   * @class TaskRunner
   * @description WebAudio testing task runner. Manages tasks.
   */
  class TaskRunner {
    constructor() {
      this._tasks = {};
      this._taskSequence = [];

      // Configure testharness.js for the async operation.
      setup(new Function(), {explicit_done: true});
    }

    _finish() {
      let numberOfFailures = 0;
      for (let taskIndex in this._taskSequence) {
        let task = this._tasks[this._taskSequence[taskIndex]];
        numberOfFailures += task.result ? 0 : 1;
      }

      let prefix = '# AUDIT TASK RUNNER FINISHED: ';
      if (numberOfFailures > 0) {
        _logFailed(
            prefix + numberOfFailures + ' out of ' + this._taskSequence.length +
            ' tasks were failed.');
      } else {
        _logPassed(
            prefix + this._taskSequence.length + ' tasks ran successfully.');
      }

      return Promise.resolve();
    }

    // |taskLabel| can be either a string or a dictionary. See Task constructor
    // for the detail.  If |taskFunction| returns a thenable, then the task
    // is considered complete when the thenable is fulfilled; otherwise the
    // task must be completed with an explicit call to |task.done()|.
    define(taskLabel, taskFunction) {
      let task = new Task(this, taskLabel, taskFunction);
      if (this._tasks.hasOwnProperty(task.label)) {
        _throwException('Audit.define:: Duplicate task definition.');
        return;
      }
      this._tasks[task.label] = task;
      this._taskSequence.push(task.label);
    }

    // Start running all the tasks scheduled. Multiple task names can be passed
    // to execute them sequentially. Zero argument will perform all defined
    // tasks in the order of definition.
    run() {
      // Display the beginning of the test suite.
      _logPassed('# AUDIT TASK RUNNER STARTED.');

      // If the argument is specified, override the default task sequence with
      // the specified one.
      if (arguments.length > 0) {
        this._taskSequence = [];
        for (let i = 0; i < arguments.length; i++) {
          let taskLabel = arguments[i];
          if (!this._tasks.hasOwnProperty(taskLabel)) {
            _throwException('Audit.run:: undefined task.');
          } else if (this._taskSequence.includes(taskLabel)) {
            _throwException('Audit.run:: duplicate task request.');
          } else {
            this._taskSequence.push(taskLabel);
          }
        }
      }

      if (this._taskSequence.length === 0) {
        _throwException('Audit.run:: no task to run.');
        return;
      }

      for (let taskIndex in this._taskSequence) {
        let task = this._tasks[this._taskSequence[taskIndex]];
        // Some tests assume that tasks run in sequence, which is provided by
        // promise_test().
        promise_test((t) => task.run(t), `Executing "${task.label}"`);
      }

      // Schedule a summary report on completion.
      promise_test(() => this._finish(), "Audit report");

      // From testharness.js. The harness now need not wait for more subtests
      // to be added.
      _testharnessDone();
    }
  }

  /**
   * Load file from a given URL and pass ArrayBuffer to the following promise.
   * @param  {String} fileUrl file URL.
   * @return {Promise}
   *
   * @example
   *   Audit.loadFileFromUrl('resources/my-sound.ogg').then((response) => {
   *       audioContext.decodeAudioData(response).then((audioBuffer) => {
   *           // Do something with AudioBuffer.
   *       });
   *   });
   */
  function loadFileFromUrl(fileUrl) {
    return new Promise((resolve, reject) => {
      let xhr = new XMLHttpRequest();
      xhr.open('GET', fileUrl, true);
      xhr.responseType = 'arraybuffer';

      xhr.onload = () => {
        // |status = 0| is a workaround for the run_web_test.py server. We are
        // speculating the server quits the transaction prematurely without
        // completing the request.
        if (xhr.status === 200 || xhr.status === 0) {
          resolve(xhr.response);
        } else {
          let errorMessage = 'loadFile: Request failed when loading ' +
              fileUrl + '. ' + xhr.statusText + '. (status = ' + xhr.status +
              ')';
          if (reject) {
            reject(errorMessage);
          } else {
            new Error(errorMessage);
          }
        }
      };

      xhr.onerror = (event) => {
        let errorMessage =
            'loadFile: Network failure when loading ' + fileUrl + '.';
        if (reject) {
          reject(errorMessage);
        } else {
          new Error(errorMessage);
        }
      };

      xhr.send();
    });
  }

  /**
   * @class Audit
   * @description A WebAudio layout test task manager.
   * @example
   *   let audit = Audit.createTaskRunner();
   *   audit.define('first-task', function (task, should) {
   *     should(someValue).beEqualTo(someValue);
   *     task.done();
   *   });
   *   audit.run();
   */
  return {

    /**
     * Creates an instance of Audit task runner.
     * @param {Object}  options                     Options for task runner.
     * @param {Boolean} options.requireResultFile   True if the test suite
     *                                              requires explicit text
     *                                              comparison with the expected
     *                                              result file.
     */
    createTaskRunner: function(options) {
      if (options && options.requireResultFile == true) {
        _logError(
            'this test requires the explicit comparison with the ' +
            'expected result when it runs with run_web_tests.py.');
      }

      return new TaskRunner();
    },

    /**
     * Load file from a given URL and pass ArrayBuffer to the following promise.
     * See |loadFileFromUrl| method for the detail.
     */
    loadFileFromUrl: loadFileFromUrl

  };

})();
