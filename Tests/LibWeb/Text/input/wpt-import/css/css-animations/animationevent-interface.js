(function() {
  test(function() {
    var event = new AnimationEvent("");
    assert_true(event instanceof window.AnimationEvent);
  }, "the event is an instance of AnimationEvent");

  test(function() {
    var event = new AnimationEvent("");
    assert_true(event instanceof window.Event);
  }, "the event inherts from Event");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent();
    }, 'First argument is required, so was expecting a TypeError.');
  }, 'Missing type argument');

  test(function() {
    var event = new AnimationEvent("test");
    assert_equals(event.type, "test");
  }, "type argument is string");

  test(function() {
    var event = new AnimationEvent(null);
    assert_equals(event.type, "null");
  }, "type argument is null");

  test(function() {
    var event = new AnimationEvent(undefined);
    assert_equals(event.type, "undefined");
  }, "event type set to undefined");

  test(function() {
    var event = new AnimationEvent("test");
    assert_equals(event.animationName, "");
  }, "animationName has default value of empty string");

  test(function() {
    var event = new AnimationEvent("test");
    assert_equals(event.elapsedTime, 0.0);
  }, "elapsedTime has default value of 0.0");

  test(function() {
    var event = new AnimationEvent("test");
    assert_readonly(event, "animationName", "readonly attribute value");
  }, "animationName is readonly");

  test(function() {
    var event = new AnimationEvent("test");
    assert_readonly(event, "elapsedTime", "readonly attribute value");
  }, "elapsedTime is readonly");

  test(function() {
    var event = new AnimationEvent("test", null);
    assert_equals(event.animationName, "");
    assert_equals(event.elapsedTime, 0.0);
  }, "animationEventInit argument is null");

  test(function() {
    var event = new AnimationEvent("test", undefined);
    assert_equals(event.animationName, "");
    assert_equals(event.elapsedTime, 0.0);
  }, "animationEventInit argument is undefined");

  test(function() {
    var event = new AnimationEvent("test", {});
    assert_equals(event.animationName, "");
    assert_equals(event.elapsedTime, 0.0);
  }, "animationEventInit argument is empty dictionary");

  test(function() {
    var event = new AnimationEvent("test", {pseudoElement: "::testPseudo"});
    assert_equals(event.pseudoElement, "::testPseudo");
  }, "AnimationEvent.pseudoElement initialized from the dictionary");

  test(function() {
    var event = new AnimationEvent("test", {animationName: "sample"});
    assert_equals(event.animationName, "sample");
  }, "animationName set to 'sample'");

  test(function() {
    var event = new AnimationEvent("test", {animationName: undefined});
    assert_equals(event.animationName, "");
  }, "animationName set to undefined");

  test(function() {
    var event = new AnimationEvent("test", {animationName: null});
    assert_equals(event.animationName, "null");
  }, "animationName set to null");

  test(function() {
    var event = new AnimationEvent("test", {animationName: false});
    assert_equals(event.animationName, "false");
  }, "animationName set to false");

  test(function() {
    var event = new AnimationEvent("test", {animationName: true});
    assert_equals(event.animationName, "true");
  }, "animationName set to true");

  test(function() {
    var event = new AnimationEvent("test", {animationName: 0.5});
    assert_equals(event.animationName, "0.5");
  }, "animationName set to a number");

  test(function() {
    var event = new AnimationEvent("test", {animationName: []});
    assert_equals(event.animationName, "");
  }, "animationName set to []");

  test(function() {
    var event = new AnimationEvent("test", {animationName: [1, 2, 3]});
    assert_equals(event.animationName, "1,2,3");
  }, "animationName set to [1, 2, 3]");

  test(function() {
    var event = new AnimationEvent("test", {animationName: {sample: 0.5}});
    assert_equals(event.animationName, "[object Object]");
  }, "animationName set to an object");

  test(function() {
    var event = new AnimationEvent("test",
        {animationName: {valueOf: function () { return 'sample'; }}});
    assert_equals(event.animationName, "[object Object]");
  }, "animationName set to an object with a valueOf function");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: 0.5});
    assert_equals(event.elapsedTime, 0.5);
  }, "elapsedTime set to 0.5");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: -0.5});
    assert_equals(event.elapsedTime, -0.5);
  }, "elapsedTime set to -0.5");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: undefined});
    assert_equals(event.elapsedTime, 0);
  }, "elapsedTime set to undefined");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: null});
    assert_equals(event.elapsedTime, 0);
  }, "elapsedTime set to null");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: false});
    assert_equals(event.elapsedTime, 0);
  }, "elapsedTime set to false");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: true});
    assert_equals(event.elapsedTime, 1);
  }, "elapsedTime set to true");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: ""});
    assert_equals(event.elapsedTime, 0);
  }, "elapsedTime set to ''");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: []});
    assert_equals(event.elapsedTime, 0);
  }, "elapsedTime set to []");

  test(function() {
    var event = new AnimationEvent("test", {elapsedTime: [0.5]});
    assert_equals(event.elapsedTime, 0.5);
  }, "elapsedTime set to [0.5]");

  test(function() {
    var event = new AnimationEvent(
        "test", {elapsedTime: { valueOf: function() { return 0.5; }}});
    assert_equals(event.elapsedTime, 0.5);
  }, "elapsedTime set to an object with a valueOf function");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: NaN});
    }, 'elapsedTime cannot be NaN so was expecting a TypeError');
  }, "elapsedTime cannot be set to NaN");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: Infinity});
    }, 'elapsedTime cannot be Infinity so was expecting a TypeError');
  }, "elapsedTime cannot be set to Infinity");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: -Infinity});
    }, 'elapsedTime cannot be -Infinity so was expecting a TypeError');
  }, "elapsedTime cannot be set to -Infinity");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: "sample"});
    }, 'elapsedTime cannot be a string so was expecting a TypeError');
  }, "elapsedTime cannot be set to 'sample'");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: [0.5, 1.0]});
    }, 'elapsedTime cannot be a multi-element array so was expecting a TypeError');
  }, "elapsedTime cannot be set to [0.5, 1.0]");

  test(function() {
    assert_throws_js(TypeError, function() {
      new AnimationEvent("test", {elapsedTime: { sample: 0.5}});
    }, 'elapsedTime cannot be an object so was expecting a TypeError');
  }, "elapsedTime cannot be set to an object");

  test(function() {
    var eventInit = {animationName: "sample", elapsedTime: 0.5};
    var event = new AnimationEvent("test", eventInit);
    assert_equals(event.animationName, "sample");
    assert_equals(event.elapsedTime, 0.5);
  }, "AnimationEventInit properties set value");

  test(function() {
    var target = document.getElementById('test');
    target.style.animation = 'sample 1s';
    var anim = target.getAnimations()[0];

    var eventInit = {animation: anim};
    var event = new AnimationEvent("test", eventInit);
    assert_equals(event.animation, anim);
  }, "AnimationEventInit animation property sets value");
})();
