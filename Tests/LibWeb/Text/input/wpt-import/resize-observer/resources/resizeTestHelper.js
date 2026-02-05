'use strict';

/**
  ResizeTestHelper is a framework to test ResizeObserver
  notifications. Use it to make assertions about ResizeObserverEntries.
  This framework is needed because ResizeObservations are
  delivered asynchronously inside the event loop.

  Features:
  - can queue multiple notification steps in a test
  - handles timeouts
  - returns Promise that is fulfilled when test completes.
    Use to chain tests (since parallel async ResizeObserver tests
    would conflict if reusing same DOM elements).

  Usage:

  create ResizeTestHelper for every test.
  Make assertions inside notify, timeout callbacks.
  Start tests with helper.start()
  Chain tests with Promises.
  Counts animation frames, see startCountingRaf
*/

/*
  @param name: test name
  @param steps:
  {
    setup: function(ResizeObserver) {
      // called at the beginning of the test step
      // your observe/resize code goes here
    },
    notify: function(entries, observer) {
      // ResizeObserver callback.
      // Make assertions here.
      // Return true if next step should start on the next event loop.
    },
    timeout: function() {
      // Define this if your test expects to time out, and the expected timeout
      // value will be 100ms.
      // If undefined, timeout is assert_unreached after 1000ms.
    }
  }
*/
function ResizeTestHelper(name, steps)
{
    this._name = name;
    this._steps = steps || [];
    this._stepIdx = -1;
    this._harnessTest = null;
    this._observer = new ResizeObserver(this._handleNotification.bind(this));
    this._timeoutBind = this._handleTimeout.bind(this);
    this._nextStepBind = this._nextStep.bind(this);
}

// The default timeout value in ms.
// We expect TIMEOUT to be longer than we would ever have to wait for notify()
// to be fired. This is used for tests which are not expected to time out, so
// it can be large without slowing down the test.
ResizeTestHelper.TIMEOUT = 1000;
// A reasonable short timeout value in ms.
// We expect SHORT_TIMEOUT to be long enough that notify() would usually get a
// chance to fire before SHORT_TIMEOUT expires. This is used for tests which
// *are* expected to time out (with no callbacks fired), so we'd like to keep
// it relatively short to avoid slowing down the test.
ResizeTestHelper.SHORT_TIMEOUT = 100;

ResizeTestHelper.prototype = {
  get _currentStep() {
    return this._steps[this._stepIdx];
  },

  _nextStep: function() {
    if (++this._stepIdx == this._steps.length)
      return this._done();
    // Use SHORT_TIMEOUT if this step expects timeout.
    let timeoutValue = this._steps[this._stepIdx].timeout ?
        ResizeTestHelper.SHORT_TIMEOUT :
        ResizeTestHelper.TIMEOUT;
    this._timeoutId = this._harnessTest.step_timeout(
      this._timeoutBind, timeoutValue);
    try {
      this._steps[this._stepIdx].setup(this._observer);
    }
    catch(err) {
      this._harnessTest.step(() => {
        assert_unreached("Caught a throw, possible syntax error");
      });
    }
  },

  _handleNotification: function(entries) {
    if (this._timeoutId) {
      window.clearTimeout(this._timeoutId);
      delete this._timeoutId;
    }
    this._harnessTest.step(() => {
      try {
        let rafDelay = this._currentStep.notify(entries, this._observer);
        if (rafDelay)
          window.requestAnimationFrame(this._nextStepBind);
        else
          this._nextStep();
      }
      catch(err) {
        this._harnessTest.step(() => {
          throw err;
        });
        // Force to _done() the current test.
        this._done();
      }
    });
  },

  _handleTimeout: function() {
    delete this._timeoutId;
    this._harnessTest.step(() => {
      if (this._currentStep.timeout) {
        this._currentStep.timeout();
      }
      else {
        this._harnessTest.step(() => {
          assert_unreached("Timed out waiting for notification. (" + ResizeTestHelper.TIMEOUT + "ms)");
        });
      }
      this._nextStep();
    });
  },

  _done: function() {
    this._observer.disconnect();
    delete this._observer;
    this._harnessTest.done();
    if (this._rafCountRequest) {
      window.cancelAnimationFrame(this._rafCountRequest);
      delete this._rafCountRequest;
    }
    window.requestAnimationFrame(() => { this._resolvePromise(); });
  },

  start: function(cleanup) {
    this._harnessTest = async_test(this._name);

    if (cleanup) {
      this._harnessTest.add_cleanup(cleanup);
    }

    this._harnessTest.step(() => {
      assert_equals(this._stepIdx, -1, "start can only be called once");
      this._nextStep();
    });
    return new Promise( (resolve, reject) => {
      this._resolvePromise = resolve;
      this._rejectPromise = reject;
    });
  },

  get rafCount() {
    if (!this._rafCountRequest)
      throw "rAF count is not active";
    return this._rafCount;
  },

  get test() {
    if (!this._harnessTest) {
      throw "_harnessTest is not initialized";
    }
    return this._harnessTest;
  },

  _incrementRaf: function() {
    if (this._rafCountRequest) {
      this._rafCount++;
      this._rafCountRequest = window.requestAnimationFrame(this._incrementRafBind);
    }
  },

  startCountingRaf: function() {
    if (this._rafCountRequest)
      window.cancelAnimationFrame(this._rafCountRequest);
    if (!this._incrementRafBind)
      this._incrementRafBind = this._incrementRaf.bind(this);
    this._rafCount = 0;
    this._rafCountRequest = window.requestAnimationFrame(this._incrementRafBind);
  }
}

function createAndAppendElement(tagName, parent) {
  if (!parent) {
    parent = document.body;
  }
  const element = document.createElement(tagName);
  parent.appendChild(element);
  return element;
}
