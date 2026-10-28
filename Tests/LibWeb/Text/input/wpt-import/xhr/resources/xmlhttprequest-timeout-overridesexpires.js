if (this.document === undefined)
  importScripts("xmlhttprequest-timeout.js");
/*
        Starts three requests:
        1) XHR to resource which will take a least TIME_XHR_LOAD ms with timeout initially set to TIME_NORMAL_LOAD ms. After TIME_LATE_TIMEOUT ms timeout is supposedly reset to TIME_DELAY ms,
           but the resource should have finished loading already. Asserts "load" fires.
        2) XHR with initial timeout set to TIME_NORMAL_LOAD, after TIME_REGULAR_TIMEOUT sets timeout to TIME_DELAY+100. Asserts "timeout" fires.
        3) XHR with initial timeout set to TIME_DELAY, after TIME_REGULAR_TIMEOUT sets timeout to 500ms. Asserts "timeout" fires (the change happens when timeout already fired and the request is done).
*/
runTestRequests([ ["RequestTracker", true, "timeout set to expiring value after load fires", TIME_NORMAL_LOAD, TIME_LATE_TIMEOUT, TIME_DELAY],
                  ["RequestTracker", true, "timeout set to expired value before load fires", TIME_NORMAL_LOAD, TIME_REGULAR_TIMEOUT, TIME_DELAY+100],
                  ["RequestTracker", true, "timeout set to non-expiring value after timeout fires", TIME_DELAY, TIME_REGULAR_TIMEOUT, 500] ]);
