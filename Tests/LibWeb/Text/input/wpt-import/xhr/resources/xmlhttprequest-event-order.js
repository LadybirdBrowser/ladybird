(function(global) {
  var recorded_xhr_events = [];

  function record_xhr_event(e) {
    var prefix = e.target instanceof XMLHttpRequestUpload ? "upload." : "";
    recorded_xhr_events.push((prefix || "") + e.type + "(" + e.loaded + "," + e.total + "," + e.lengthComputable + ")");
  }

  global.prepare_xhr_for_event_order_test = function(xhr) {
    xhr.addEventListener("readystatechange", function(e) {
      recorded_xhr_events.push(xhr.readyState);
    });
    var events = ["loadstart", "progress", "abort", "timeout", "error", "load", "loadend"];
    for(var i=0; i<events.length; ++i) {
      xhr.addEventListener(events[i], record_xhr_event);
    }
    if ("upload" in xhr) {
      for(var i=0; i<events.length; ++i) {
        xhr.upload.addEventListener(events[i], record_xhr_event);
      }
    }
  }

  function getNextEvent(arr) {
    var event = { str: arr.shift() };

    // we can only handle strings, numbers (readystates) and undefined
    if (event.str === undefined) {
      return event;
    }

    if (typeof event.str !== "string") {
      if (Number.isInteger(event.str)) {
        event.state = event.str;
        event.str = "readystatechange(" + event.str + ")";
      } else {
        throw "Test error: unexpected event type " + event.str;
      }
    }

    // parse out the general type, loaded and total values
    var type = event.type = event.str.split("(")[0].split(".").pop();
    var loadedAndTotal = event.str.match(/.*\((\d+),(\d+),(true|false)\)/);
    if (loadedAndTotal) {
      event.loaded = parseInt(loadedAndTotal[1]);
      event.total = parseInt(loadedAndTotal[2]);
      event.lengthComputable = loadedAndTotal[3] == "true";
    }

    return event;
  }

  global.assert_xhr_event_order_matches = function(expected) {
    var recorded = recorded_xhr_events;
    var lastRecordedLoaded = -1;
    while(expected.length && recorded.length) {
      var currentExpected = getNextEvent(expected),
          currentRecorded = getNextEvent(recorded);

      // skip to the last progress event if we've hit one (note the next
      // event after a progress event should be a LOADING readystatechange,
      // if there are multiple progress events in a row).
      while (recorded.length && currentRecorded.type == "progress" &&
             parseInt(recorded) === 3) {
        assert_greater_than(currentRecorded.loaded, lastRecordedLoaded,
                            "progress event 'loaded' values must only increase");
        lastRecordedLoaded = currentRecorded.loaded;
      }
      if (currentRecorded.type == "loadend") {
        recordedProgressCount = 0;
        lastRecordedLoaded = -1;
      }

      assert_equals(currentRecorded.str, currentExpected.str);
    }
    if (recorded.length) {
      throw "\nUnexpected extra events: " + recorded.join(", ");
    }
    if (expected.length) {
      throw "\nExpected more events: " + expected.join(", ");
    }
  }
}(this));
