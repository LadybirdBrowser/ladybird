// Check a Touch object's attributes for existence and correct type
// TA: 1.1.2, 1.1.3
function check_Touch_object(t) {
    assert_equals(Object.prototype.toString.call(t), "[object Touch]", "touch is of type Touch");
    [
        ["long", "identifier"],
        ["EventTarget", "target"],
        ["long", "screenX"],
        ["long", "screenY"],
        ["long", "clientX"],
        ["long", "clientY"],
        ["long", "pageX"],
        ["long", "pageY"],
        ["long", "radiusX"],
        ["long", "radiusY"],
        ["long", "rotationAngle"],
        ["long", "force"],
    ].forEach(function(attr) {
        var type = attr[0];
        var name = attr[1];

        // existence check
        assert_true(name in t, name + " attribute in Touch object");

        // type check
        switch (type) {
            case "long":
                assert_equals(typeof t[name], "number", name + " attribute of type long");
                break;
            case "EventTarget":
                // An event target is some type of Element
                assert_true(t[name] instanceof Element, "EventTarget must be an Element.");
                break;
            default:
                break;
        }
    });
}

// Check a TouchList object's attributes and methods for existence and proper type
// Also make sure all of the members of the list are Touch objects
// TA: 1.2.1, 1.2.2, 1.2.5, 1.2.6
function check_TouchList_object(tl) {
    assert_equals(Object.prototype.toString.call(tl), "[object TouchList]", "touch list is of type TouchList");
    [
        ["unsigned long", "length"],
        ["function", "item"],
    ].forEach(function(attr) {
        var type = attr[0];
        var name = attr[1];

        // existence check
        assert_true(name in tl, name + " attribute in TouchList");

        // type check
        switch (type) {
            case "unsigned long":
                assert_equals(typeof tl[name], "number", name + " attribute of type long");
                break;
            case "function":
                assert_equals(typeof tl[name], "function", name + " attribute of type function");
                break;
            default:
                break;
        }
    });
    // Each member of tl should be a proper Touch object
    for (var i = 0; i < tl.length; i++) {
        check_Touch_object(tl.item(i));
    }
    // TouchList.item(x) should return null if x is >= TouchList.length
    var t = tl.item(tl.length);
    assert_equals(t, null, "TouchList.item returns null if the index is >= the length of the list");
}

// Check a TouchEvent event's attributes for existence and proper type
// Also check that each of the event's TouchList objects are valid
// TA: 1.{3,4,5}.1.1, 1.{3,4,5}.1.2
function check_TouchEvent(ev) {
    assert_true(ev instanceof TouchEvent, ev.type + " event is a TouchEvent event");
    [
        ["TouchList", "touches"],
        ["TouchList", "targetTouches"],
        ["TouchList", "changedTouches"],
        ["boolean", "altKey"],
        ["boolean", "metaKey"],
        ["boolean", "ctrlKey"],
        ["boolean", "shiftKey"],
    ].forEach(function(attr) {
        var type = attr[0];
        var name = attr[1];
        // existence check
        assert_true(name in ev, name + " attribute in " + ev.type + " event");
        // type check
        switch (type) {
            case "boolean":
                assert_equals(typeof ev[name], "boolean", name + " attribute of type boolean");
                break;
            case "TouchList":
                assert_equals(Object.prototype.toString.call(ev[name]), "[object TouchList]", name + " attribute of type TouchList");
                break;
            default:
                break;
        }
    });
}

// This chromium-specific helper is a no-op to other user-agents. It can be used
// to ensure that chromium's input-handling compositor thread is ready before
// touch-related test logic proceeds.
// TODO(crbug.com/41481669): This shouldn't be necessary if the test harness
// deferred running the tests until after paint holding.
async function waitTillReadyForTouchInput() {
  const animation =
    document.body.animate({ opacity: [ 0, 1 ] }, {duration: 1 });
  return animation.finished;
}
