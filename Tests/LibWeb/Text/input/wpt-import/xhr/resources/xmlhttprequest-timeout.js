/* Test adapted from Alex Vincent's XHR2 timeout tests, written for Mozilla.
   https://hg.mozilla.org/mozilla-central/file/tip/content/base/test/
   Released into the public domain or under BSD, according to
   https://bugzilla.mozilla.org/show_bug.cgi?id=525816#c86
*/

/* Notes:
   - All times are expressed in milliseconds in this test suite.
   - Test harness code is at the end of this file.
   - We generate only one request at a time, to avoid overloading the HTTP
   request handlers.
 */

var TIME_NORMAL_LOAD = 5000;
var TIME_LATE_TIMEOUT = 4000;
var TIME_XHR_LOAD = 3000;
var TIME_REGULAR_TIMEOUT = 2000;
var TIME_SYNC_TIMEOUT = 1000;
var TIME_DELAY = 1000;

/*
 * This should point to a resource that responds with a text/plain resource after a delay of TIME_XHR_LOAD milliseconds.
 */
var STALLED_REQUEST_URL = "delay.py?ms=" + (TIME_XHR_LOAD);

var inWorker = false;
try {
  inWorker = !(self instanceof Window);
} catch (e) {
  inWorker = true;
}

if (!inWorker)
  STALLED_REQUEST_URL = "resources/" + STALLED_REQUEST_URL;

function message(obj) {
  if (inWorker)
    self.postMessage(obj);
  else
    self.postMessage(obj, "*");
}

function is(got, expected, msg) {
  var obj = {};
  obj.type = "is";
  obj.got = got;
  obj.expected = expected;
  obj.msg = msg;

  message(obj);
}

function ok(bool, msg) {
  var obj = {};
  obj.type = "ok";
  obj.bool = bool;
  obj.msg = msg;

  message(obj);
}

/**
 * Generate and track results from a XMLHttpRequest with regards to timeouts.
 *
 * @param {String} id         The test description.
 * @param {Number} timeLimit  The initial setting for the request timeout.
 * @param {Number} resetAfter (Optional) The time after sending the request, to
 *                            reset the timeout.
 * @param {Number} resetTo    (Optional) The delay to reset the timeout to.
 *
 * @note The actual testing takes place in handleEvent(event).
 * The requests are generated in startXHR().
 *
 * @note If resetAfter and resetTo are omitted, only the initial timeout setting
 * applies.
 *
 * @constructor
 * @implements DOMEventListener
 */
function RequestTracker(async, id, timeLimit /*[, resetAfter, resetTo]*/) {
  this.async = async;
  this.id = id;
  this.timeLimit = timeLimit;

  if (arguments.length > 3) {
    this.mustReset  = true;
    this.resetAfter = arguments[3];
    this.resetTo    = arguments[4];
  }

  this.hasFired = false;
}
RequestTracker.prototype = {
  /**
   * Start the XMLHttpRequest!
   */
  startXHR: function() {
    var req = new XMLHttpRequest();
    this.request = req;
    req.open("GET", STALLED_REQUEST_URL, this.async);
    var me = this;
    function handleEvent(e) { return me.handleEvent(e); };
    req.onerror = handleEvent;
    req.onload = handleEvent;
    req.onabort = handleEvent;
    req.ontimeout = handleEvent;

    req.timeout = this.timeLimit;

    if (this.mustReset) {
      var resetTo = this.resetTo;
      self.setTimeout(function() {
        req.timeout = resetTo;
      }, this.resetAfter);
    }

    try {
      req.send(null);
    }
    catch (e) {
      // Synchronous case in workers.
      ok(!this.async && this.timeLimit < TIME_XHR_LOAD && e.name == "TimeoutError", "Unexpected error: " + e);
      TestCounter.testComplete();
    }
  },

  /**
   * Get a message describing this test.
   *
   * @returns {String} The test description.
   */
  getMessage: function() {
    var rv = this.id + ", ";
    if (this.mustReset) {
      rv += "original timeout at " + this.timeLimit + ", ";
      rv += "reset at " + this.resetAfter + " to " + this.resetTo;
    }
    else {
      rv += "timeout scheduled at " + this.timeLimit;
    }
    return rv;
  },

  /**
   * Check the event received, and if it's the right (and only) one we get.
   *
   * @param {DOMProgressEvent} evt An event of type "load" or "timeout".
   */
  handleEvent: function(evt) {
    if (this.hasFired) {
      ok(false, "Only one event should fire: " + this.getMessage());
      return;
    }
    this.hasFired = true;

    var type = evt.type, expectedType;
    // The XHR responds after TIME_XHR_LOAD milliseconds with a load event.
    var timeLimit = this.mustReset && (this.resetAfter < Math.min(TIME_XHR_LOAD, this.timeLimit)) ?
                    this.resetTo :
                    this.timeLimit;
    if ((timeLimit == 0) || (timeLimit >= TIME_XHR_LOAD)) {
      expectedType = "load";
    }
    else {
      expectedType = "timeout";
    }
    is(type, expectedType, this.getMessage());
    TestCounter.testComplete();
  }
};

/**
 * Generate and track XMLHttpRequests which will have abort() called on.
 *
 * @param shouldAbort {Boolean} True if we should call abort at all.
 * @param abortDelay  {Number}  The time in ms to wait before calling abort().
 */
function AbortedRequest(shouldAbort, id, abortDelay) {
  this.shouldAbort = shouldAbort;
  this.abortDelay  = abortDelay;
  this.hasFired    = false;
}
AbortedRequest.prototype = {
  /**
   * Start the XMLHttpRequest!
   */
  startXHR: function() {
    var req = new XMLHttpRequest();
    this.request = req;
    req.open("GET", STALLED_REQUEST_URL);
    var _this = this;
    function handleEvent(e) { return _this.handleEvent(e); };
    req.onerror = handleEvent;
    req.onload = handleEvent;
    req.onabort = handleEvent;
    req.ontimeout = handleEvent;

    req.timeout = TIME_REGULAR_TIMEOUT;

    function abortReq() {
      req.abort();
    }

    if (!this.shouldAbort) {
      self.setTimeout(function() {
        try {
          _this.noEventsFired();
        }
        catch (e) {
          ok(false, "Unexpected error: " + e);
          TestCounter.testComplete();
        }
      }, TIME_NORMAL_LOAD);
    }
    else {
      // Abort events can only be triggered on sent requests.
      req.send();
      if (this.abortDelay == -1) {
        abortReq();
      }
      else {
        self.setTimeout(abortReq, this.abortDelay);
      }
    }
  },

  /**
   * Ensure that no events fired at all, especially not our timeout event.
   */
  noEventsFired: function() {
    ok(!this.hasFired, "No events should fire for an unsent, unaborted request");
    // We're done; if timeout hasn't fired by now, it never will.
    TestCounter.testComplete();
  },

  /**
   * Get a message describing this test.
   *
   * @returns {String} The test description.
   */
  getMessage: function() {
    return "time to abort is " + this.abortDelay + ", timeout set at " + TIME_REGULAR_TIMEOUT;
  },

  /**
   * Check the event received, and if it's the right (and only) one we get.
   *
   * WebKit fires abort events even for DONE and UNSENT states, which is
   * discussed in http://webkit.org/b/98404
   * That's why we chose to accept secondary "abort" events in this test.
   *
   * @param {DOMProgressEvent} evt An event of type "load" or "timeout".
   */
  handleEvent: function(evt) {
    if (this.hasFired && evt.type != "abort") {
      ok(false, "Only abort event should fire: " + this.getMessage());
      return;
    }

    var expectedEvent = (this.abortDelay >= TIME_REGULAR_TIMEOUT && !this.hasFired) ? "timeout" : "abort";
    this.hasFired = true;
    is(evt.type, expectedEvent, this.getMessage());
    TestCounter.testComplete();
  }
};

function SyncRequestSettingTimeoutAfterOpen() {
  this.startXHR = function() {
    var pass = false;
    var req = new XMLHttpRequest();
    req.open("GET", STALLED_REQUEST_URL, false);
    try {
      req.timeout = TIME_SYNC_TIMEOUT;
    }
    catch (e) {
      pass = true;
    }
    ok(pass, "Synchronous XHR must not allow a timeout to be set - setting timeout must throw");
    TestCounter.testComplete();
  };
  return this;
};

function SyncRequestSettingTimeoutBeforeOpen() {
  this.startXHR = function() {
    var pass = false;
    var req = new XMLHttpRequest();
    req.timeout = TIME_SYNC_TIMEOUT;
    try {
      req.open("GET", STALLED_REQUEST_URL, false);
    }
    catch (e) {
      pass = true;
    }
    ok(pass, "Synchronous XHR must not allow a timeout to be set - calling open() after timeout is set must throw");
    TestCounter.testComplete();
  }
  return this;
};

var TestRequests = [];

// This code controls moving from one test to another.
var TestCounter = {
  testComplete: function() {
    // Allow for the possibility there are other events coming.
    self.setTimeout(function() {
      TestCounter.next();
    }, TIME_NORMAL_LOAD);
  },

  next: function() {
    var test = TestRequests.shift();

    if (test) {
      test.startXHR();
    }
    else {
      message("done");
    }
  }
};

function runTestRequests(testRequests) {
  if (location.search) {
    testRequests = testRequests.filter(test => test[2] == decodeURIComponent(location.search.substr(1)));
  }
  TestRequests = testRequests.map(test => {
    var constructor = test.shift();
    return new self[constructor](...test)
  });
  TestCounter.next();
}
