const All_Pointer_Events = [
  "pointerdown",
  "pointerup",
  "pointercancel",
  "pointermove",
  "pointerover",
  "pointerout",
  "pointerenter",
  "pointerleave",
  "gotpointercapture",
  "lostpointercapture"
];

// https://w3c.github.io/pointerevents/#the-button-property
// Values for the button property, which indicates the device button whose state
// change fired the event.
const ButtonChange = {
  NONE: -1,
  PEN_CONTACT: 0,
  TOUCH_CONTACT: 0,
  LEFT_MOUSE: 0,
  MIDDLE_MOUSE: 1,
  RIGHT_MOUSE: 2,
  X1_MOUSE: 3,
  X2_MOUSE: 4,
  PEN_ERASER_BUTTON: 5
};

// https://w3c.github.io/pointerevents/#the-buttons-property
// The buttons property gives the current state of the device buttons as a
// bitmask.
const ButtonsBitfield = {
  NONE: 0,
  PEN_CONTACT: 1,
  TOUCH_CONTACT: 1,
  LEFT_MOUSE: 1,
  RIGHT_MOUSE: 2,
  PEN_BARREL_BUTTON: 2,
  MIDDLE_MOUSE: 4,
  X1_MOUSE: 8,
  X2_MOUSE: 16,
  PEN_ERASER_BUTTON: 32
};

// Check for conformance to PointerEvent interface
// https://w3c.github.io/pointerevents/#pointerevent-interface
function check_PointerEvent(event, testNamePrefix, standardAttrs = true) {
  if (testNamePrefix === undefined)
    testNamePrefix = "";

  // Use expectedPointerType if set otherwise just use the incoming event pointerType in the test name.
  var pointerTestName = (testNamePrefix ? testNamePrefix + ' ' : '')
    + (expectedPointerType == null ? event.pointerType : expectedPointerType) + ' ' + event.type;

  if (standardAttrs) {
    if (expectedPointerType != null) {
      test(function () {
        assert_equals(event.pointerType, expectedPointerType);
      }, pointerTestName + ".pointerType is correct.");
    }

    test(function () {
      assert_true(event instanceof event.target.ownerDocument.defaultView.PointerEvent);
    }, pointerTestName + " event is a PointerEvent event");
  }

  // Check attributes for conformance to WebIDL (existence, type, being readable).
  var idl_type_check = {
    "long": function (v) { return typeof v === "number" && Math.round(v) === v; },
    "float": function (v) { return typeof v === "number"; },
    "string": function (v) { return typeof v === "string"; },
    "boolean": function (v) { return typeof v === "boolean" },
    "object": function (v) { return typeof v === "object" }
  };

  // Check values for inherited attributes.
  // https://w3c.github.io/pointerevents/#attributes-and-default-actions
  if (!standardAttrs) {
    test(function () {
      assert_implements_optional("fromElement" in event);
      assert_equals(event.fromElement, null);
    }, pointerTestName + ".fromElement value is null");
    test(function () {
      assert_implements_optional("toElement" in event);
      assert_equals(event.toElement, null);
    }, pointerTestName + ".toElement value is null");
  } else {
    test(function () {
      assert_equals(event.isTrusted, true);
    }, pointerTestName + ".isTrusted value is true");
    test(function () {
      let expected = (event.type != 'pointerenter' && event.type != 'pointerleave');
      assert_equals(event.composed, expected);
    }, pointerTestName + ".composed value is valid");
    test(function () {
      let expected = (event.type != 'pointerenter' && event.type != 'pointerleave');
      assert_equals(event.bubbles, expected);
    }, pointerTestName + ".bubbles value is valid");
    test(function () {
      let cancelable_events = [
        'pointerdown', 'pointermove', 'pointerup', 'pointerover', 'pointerout'
      ];
      assert_equals(event.cancelable, cancelable_events.includes(event.type));
    }, pointerTestName + ".cancelable value is valid");

    // Check the pressure value.
    // https://w3c.github.io/pointerevents/#dom-pointerevent-pressure
    test(function () {
      assert_greater_than_equal(event.pressure, 0, "pressure is greater than or equal to 0");
      assert_less_than_equal(event.pressure, 1, "pressure is less than or equal to 1");

      if (event.buttons === 0) {
        assert_equals(event.pressure, 0, "pressure is 0 with no buttons pressed");
      } else {
        assert_greater_than(event.pressure, 0, "pressure is greater than 0 with a button pressed");
        if (event.pointerType === "mouse") {
          assert_equals(event.pressure, 0.5, "pressure is 0.5 for mouse with a button pressed");
        }
      }
    }, pointerTestName + ".pressure value is valid");

    // Check mouse-specific properties.
    if (event.pointerType === "mouse") {
      test(function () {
        assert_equals(event.width, 1, "width of mouse should be 1");
        assert_equals(event.height, 1, "height of mouse should be 1");
        assert_equals(event.tiltX, 0, event.type + ".tiltX is 0 for mouse");
        assert_equals(event.tiltY, 0, event.type + ".tiltY is 0 for mouse");
        assert_true(event.isPrimary, event.type + ".isPrimary is true for mouse");
      }, pointerTestName + " properties for pointerType = mouse");
    }

    // Check "pointerup" specific properties.
    if (event.type == "pointerup") {
      test(function () {
        assert_equals(event.width, 1, "width of pointerup should be 1");
        assert_equals(event.height, 1, "height of pointerup should be 1");
      }, pointerTestName + " properties for pointerup");
    }
  }
}

function showPointerTypes() {
  var complete_notice = document.getElementById("complete-notice");
  var pointertype_log = document.getElementById("pointertype-log");
  var pointertypes = Object.keys(detected_pointertypes);
  pointertype_log.innerHTML = pointertypes.length ?
    pointertypes.join(",") : "(none)";
  complete_notice.style.display = "block";
}

function showLoggedEvents() {
  var event_log_elem = document.getElementById("event-log");
  event_log_elem.innerHTML = event_log.length ? event_log.join(", ") : "(none)";

  var complete_notice = document.getElementById("complete-notice");
  complete_notice.style.display = "block";
}

function failOnScroll() {
  assert_true(false,
  "scroll received while shouldn't");
}

function updateDescriptionNextStep() {
  document.getElementById('desc').innerHTML = "Test Description: Try to scroll text RIGHT.";
}

function updateDescriptionComplete() {
  document.getElementById('desc').innerHTML = "Test Description: Test complete";
}

function objectScroller(target, direction, value) {
  if (direction == 'up') {
    target.scrollTop = 0;
  } else if (direction == 'left') {
    target.scrollLeft = 0;
  }
}

function sPointerCapture(e) {
  try {
    target0.setPointerCapture(e.pointerId);
  }
  catch(e) {
  }
}

function rPointerCapture(e) {
  try {
    captureButton.value = 'Set Capture';
    target0.releasePointerCapture(e.pointerId);
  }
  catch(e) {
  }
}

var globalPointerEventTest = null;
var expectedPointerType = null;
const ALL_POINTERS = ['mouse', 'touch', 'pen'];

function MultiPointerTypeTest(testName, types) {
  this.testName = testName;
  this.types = types;
  this.currentTypeIndex = 0;
  this.currentTest = null;
  this.createNextTest();
}

MultiPointerTypeTest.prototype.step = function(op) {
  this.currentTest.step(op);
}

MultiPointerTypeTest.prototype.skip = function() {
  var prevTest = this.currentTest;
  this.createNextTest();
  prevTest.timeout();
}

MultiPointerTypeTest.prototype.done = function() {
  if (this.currentTest.status != 1) {
    var prevTest = this.currentTest;
    this.createNextTest();
    if (prevTest != null)
      prevTest.done();
  }
}

MultiPointerTypeTest.prototype.step = function(stepFunction) {
  this.currentTest.step(stepFunction);
}

MultiPointerTypeTest.prototype.createNextTest = function() {
  if (this.currentTypeIndex < this.types.length) {
    var pointerTypeDescription = document.getElementById('pointerTypeDescription');
    document.getElementById('pointerTypeDescription').innerHTML = "Follow the test instructions with <span style='color: red'>" + this.types[this.currentTypeIndex] + "</span>. If you don't have the device <a href='javascript:;' onclick='globalPointerEventTest.skip()'>skip it</a>.";
    this.currentTest = async_test(this.types[this.currentTypeIndex] + ' ' + this.testName);
    expectedPointerType = this.types[this.currentTypeIndex];
    this.currentTypeIndex++;
  } else {
    document.getElementById('pointerTypeDescription').innerHTML = "";
  }
  resetTestState();
}

function setup_pointerevent_test(testName, supportedPointerTypes) {
  return globalPointerEventTest = new MultiPointerTypeTest(testName, supportedPointerTypes);
}

function checkPointerEventType(event) {
  assert_equals(event.pointerType, expectedPointerType, "pointerType should be the same as the requested device.");
}

function touchScrollInTarget(target, direction) {
  var x_delta = 0;
  var y_delta = 0;
  if (direction == "down") {
    x_delta = 0;
    y_delta = -10;
  } else if (direction == "up") {
    x_delta = 0;
    y_delta = 10;
  } else if (direction == "right") {
    x_delta = -10;
    y_delta = 0;
  } else if (direction == "left") {
    x_delta = 10;
    y_delta = 0;
  } else {
    throw("scroll direction '" + direction + "' is not expected, direction should be 'down', 'up', 'left' or 'right'");
  }
  return new test_driver.Actions()
    .addPointer("touchPointer1", "touch")
    .pointerMove(0, 0, {origin: target})
    .pointerDown()
    .pointerMove(x_delta, y_delta, {origin: target})
    .pointerMove(2 * x_delta, 2 * y_delta, {origin: target})
    .pointerMove(3 * x_delta, 3 * y_delta, {origin: target})
    .pointerMove(4 * x_delta, 4 * y_delta, {origin: target})
    .pointerMove(5 * x_delta, 5 * y_delta, {origin: target})
    .pointerMove(6 * x_delta, 6 * y_delta, {origin: target})
    .pause(100)
    .pointerUp()
    .send();
}

function clickInTarget(pointerType, target) {
  var pointerId = pointerType + "Pointer1";
  return new test_driver.Actions()
    .addPointer(pointerId, pointerType)
    .pointerMove(0, 0, {origin: target})
    .pointerDown()
    .pointerUp()
    .send();
}

function rightClickInTarget(pointerType, target) {
  let pointerId = pointerType + "Pointer1";
  let actions = new test_driver.Actions();
  return actions.addPointer(pointerId, pointerType)
    .pointerMove(0, 0, {origin: target})
    .pointerDown({button:actions.ButtonType.RIGHT})
    .pointerUp({button:actions.ButtonType.RIGHT})
    .send();
}

function twoFingerDrag(target) {
  return new test_driver.Actions()
    .addPointer("touchPointer1", "touch")
    .addPointer("touchPointer2", "touch")
    .pointerMove(0, 0, { origin: target, sourceName: "touchPointer1" })
    .pointerMove(10, 0, { origin: target, sourceName: "touchPointer2" })
    .pointerDown({ sourceName: "touchPointer1" })
    .pointerDown({ sourceName: "touchPointer2" })
    .pointerMove(0, 10, { origin: target, sourceName: "touchPointer1" })
    .pointerMove(10, 10, { origin: target, sourceName: "touchPointer2" })
    .pointerMove(0, 20, { origin: target, sourceName: "touchPointer1" })
    .pointerMove(10, 20, { origin: target, sourceName: "touchPointer2" })
    .pause(100)
    .pointerUp({ sourceName: "touchPointer1" })
    .pointerUp({ sourceName: "touchPointer2" })
    .send();
}

function pointerDragInTarget(pointerType, target, direction) {
  var x_delta = 0;
  var y_delta = 0;
  if (direction == "down") {
    x_delta = 0;
    y_delta = 10;
  } else if (direction == "up") {
    x_delta = 0;
    y_delta = -10;
  } else if (direction == "right") {
    x_delta = 10;
    y_delta = 0;
  } else if (direction == "left") {
    x_delta = -10;
    y_delta = 0;
  } else {
    throw("drag direction '" + direction + "' is not expected, direction should be 'down', 'up', 'left' or 'right'");
  }
  var pointerId = pointerType + "Pointer1";
  return new test_driver.Actions()
    .addPointer(pointerId, pointerType)
    .pointerMove(0, 0, {origin: target})
    .pointerDown()
    .pointerMove(x_delta, y_delta, {origin: target})
    .pointerMove(2 * x_delta, 2 * y_delta, {origin: target})
    .pointerMove(3 * x_delta, 3 * y_delta, {origin: target})
    .pointerUp()
    .send();
}

function pointerHoverInTarget(pointerType, target, direction) {
  var x_delta = 0;
  var y_delta = 0;
  if (direction == "down") {
    x_delta = 0;
    y_delta = 10;
  } else if (direction == "up") {
    x_delta = 0;
    y_delta = -10;
  } else if (direction == "right") {
    x_delta = 10;
    y_delta = 0;
  } else if (direction == "left") {
    x_delta = -10;
    y_delta = 0;
  } else {
    throw("drag direction '" + direction + "' is not expected, direction should be 'down', 'up', 'left' or 'right'");
  }
  var pointerId = pointerType + "Pointer1";
  return new test_driver.Actions()
    .addPointer(pointerId, pointerType)
    .pointerMove(0, 0, {origin: target})
    .pointerMove(x_delta, y_delta, {origin: target})
    .pointerMove(2 * x_delta, 2 * y_delta, {origin: target})
    .pointerMove(3 * x_delta, 3 * y_delta, {origin: target})
    .send();
}

function moveToDocument(pointerType) {
  var pointerId = pointerType + "Pointer1";
  return new test_driver.Actions()
    .addPointer(pointerId, pointerType)
    // WebDriver initializes the pointer position (0, 0), therefore, we need
    // to move different position first.  Otherwise, moving to (0, 0) may be
    // ignored.
    .pointerMove(1, 1)
    .pointerMove(0, 0)
    .send();
}

// Returns a promise that only gets resolved when the condition is met.
function resolveWhen(condition) {
  return new Promise((resolve, reject) => {
    function tick() {
      if (condition())
        resolve();
      else
        requestAnimationFrame(tick.bind(this));
    }
    tick();
  });
}

// Returns a promise that only gets resolved after n animation frames
function waitForAnimationFrames(n) {
  let p = 0;
  function next() {
    p++;
    return p === n;
  }
  return resolveWhen(next);
}

function isPointerEvent(eventName) {
  return All_Pointer_Events.includes(eventName);
}

function isMouseEvent(eventName) {
  return ["mousedown", "mouseup", "mousemove", "mouseover",
    "mouseenter", "mouseout", "mouseleave",
    "click", "contextmenu", "dblclick"
  ].includes(eventName);
}

// Events is a list of events fired at a target.
//
// Checks to see if each pointer event has a corresponding mouse event in the
// event array and the two events are in the proper order (pointer event is
// first).
//
// See https://w3c.github.io/pointerevents/#mapping-for-devices-that-support-hover
function arePointerEventsBeforeCompatMouseEvents(events) {
  function arePointerAndMouseEventCompatible(pointerEventName, mouseEventName) {
    return pointerEventName.startsWith("pointer")
      && mouseEventName.startsWith("mouse")
      && pointerEventName.substring(7) === mouseEventName.substring(5);
  }

  function arePointerAndMouseEventInProperOrder(pointerEventIndex, mouseEventIndex, events) {
    return (pointerEventIndex < mouseEventIndex && isPointerEvent(events[pointerEventIndex]) && isMouseEvent(events[mouseEventIndex])
      && arePointerAndMouseEventCompatible(events[pointerEventIndex], events[mouseEventIndex]));
  }

  let currentPointerEventIndex = events.findIndex((event) => isPointerEvent(event));
  let currentMouseEventIndex = events.findIndex((event) => isMouseEvent(event));

  while (1) {
    if (currentMouseEventIndex < 0 && currentPointerEventIndex < 0)
      return true;
    if (currentMouseEventIndex < 0 || currentPointerEventIndex < 0)
      return false;
    if (!arePointerAndMouseEventInProperOrder(currentPointerEventIndex, currentMouseEventIndex, events))
      return false;

    let pointerIdx = events.slice(currentPointerEventIndex + 1).findIndex(isPointerEvent);
    let mouseIdx = events.slice(currentMouseEventIndex + 1).findIndex(isMouseEvent);

    currentPointerEventIndex = (pointerIdx < 0) ? pointerIdx : (currentPointerEventIndex + 1 + pointerIdx);
    currentMouseEventIndex = (mouseIdx < 0) ? mouseIdx : (currentMouseEventIndex + 1 + mouseIdx);
  }

  return true;
}

// Returns a |Promise| that gets resolved with the event object when |target|
// receives an event of type |event_type|.
//
// The optional |test| parameter adds event handler cleanup for the case |test|
// terminates before the event is received.
function getEvent(event_type, target, test) {
  return new Promise(resolve => {
    const listener = e => resolve(e);
    target.addEventListener(event_type, listener, { once: true });
    if (test) {
      test.add_cleanup(() =>
          target.removeEventListener(event_type, listener, { once: true }));
    }
  });
}

// Returns a |Promise| that gets resolved with |event.data| when |window|
// receives from |source| a "message" event whose |event.data.type| matches the
// string |message_data_type|.
//
// The optional |test| parameter adds event handler cleanup for the case |test|
// terminates before a matching event is received.
function getMessageData(message_data_type, source, test) {
  return new Promise(resolve => {
    const listener = e => {
      if (e.source != source || !e.data || e.data.type != message_data_type)
        return;
      window.removeEventListener("message", listener);
      resolve(e.data);
    }

    window.addEventListener("message", listener);
    if (test) {
      test.add_cleanup(() =>
          window.removeEventListener("message", listener));
    }
  });
}

// The optional |test| parameter adds event handler cleanup for the case |test|
// terminates before the event is received.
function preventDefaultPointerdownOnce(target, test) {
  return new Promise((resolve) => {
    const listener = e => {
      e.preventDefault();
      resolve();
    }

    target.addEventListener("pointerdown", listener, { once: true });
    if (test) {
      test.add_cleanup(() =>
          target.removeEventListener("pointerdown", listener, { once: true }));
    }
  });
}
