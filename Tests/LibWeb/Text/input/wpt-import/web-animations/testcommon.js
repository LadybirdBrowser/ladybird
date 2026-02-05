'use strict';

const MS_PER_SEC = 1000;

// The recommended minimum precision to use for time values[1].
//
// [1] https://drafts.csswg.org/web-animations/#precision-of-time-values
const TIME_PRECISION = 0.0005; // ms

// Allow implementations to substitute an alternative method for comparing
// times based on their precision requirements.
if (!window.assert_times_equal) {
  window.assert_times_equal = (actual, expected, description) => {
    assert_approx_equals(actual, expected, TIME_PRECISION * 2, description);
  };
}

// Allow implementations to substitute an alternative method for comparing
// times based on their precision requirements.
if (!window.assert_time_greater_than_equal) {
 window.assert_time_greater_than_equal = (actual, expected, description) => {
    assert_greater_than_equal(actual, expected - 2 * TIME_PRECISION,
                              description);
  };
}

// Allow implementations to substitute an alternative method for comparing
// a time value based on its precision requirements with a fixed value.
if (!window.assert_time_equals_literal) {
  window.assert_time_equals_literal = (actual, expected, description) => {
    if (Math.abs(expected) === Infinity) {
      assert_equals(actual, expected, description);
    } else {
      assert_approx_equals(actual, expected, TIME_PRECISION, description);
    }
  }
}

// creates div element, appends it to the document body and
// removes the created element during test cleanup
function createDiv(test, doc) {
  return createElement(test, 'div', doc);
}

// creates element of given tagName, appends it to the document body and
// removes the created element during test cleanup
// if tagName is null or undefined, returns div element
function createElement(test, tagName, doc) {
  if (!doc) {
    doc = document;
  }
  const element = doc.createElement(tagName || 'div');
  doc.body.appendChild(element);
  test.add_cleanup(() => {
    element.remove();
  });
  return element;
}

// Creates a style element with the specified rules, appends it to the document
// head and removes the created element during test cleanup.
// |rules| is an object. For example:
// { '@keyframes anim': '' ,
//   '.className': 'animation: anim 100s;' };
// or
// { '.className1::before': 'content: ""; width: 0px; transition: all 10s;',
//   '.className2::before': 'width: 100px;' };
// The object property name could be a keyframes name, or a selector.
// The object property value is declarations which are property:value pairs
// split by a space.
function createStyle(test, rules, doc) {
  if (!doc) {
    doc = document;
  }
  const extraStyle = doc.createElement('style');
  doc.head.appendChild(extraStyle);
  if (rules) {
    const sheet = extraStyle.sheet;
    for (const selector in rules) {
      sheet.insertRule(`${selector}{${rules[selector]}}`,
                       sheet.cssRules.length);
    }
  }
  test.add_cleanup(() => {
    extraStyle.remove();
  });
}

// Cubic bezier with control points (0, 0), (x1, y1), (x2, y2), and (1, 1).
function cubicBezier(x1, y1, x2, y2) {
  const xForT = t => {
    const omt = 1-t;
    return 3 * omt * omt * t * x1 + 3 * omt * t * t * x2 + t * t * t;
  };

  const yForT = t => {
    const omt = 1-t;
    return 3 * omt * omt * t * y1 + 3 * omt * t * t * y2 + t * t * t;
  };

  const tForX = x => {
    // Binary subdivision.
    let mint = 0, maxt = 1;
    for (let i = 0; i < 30; ++i) {
      const guesst = (mint + maxt) / 2;
      const guessx = xForT(guesst);
      if (x < guessx) {
        maxt = guesst;
      } else {
        mint = guesst;
      }
    }
    return (mint + maxt) / 2;
  };

  return x => {
    if (x == 0) {
      return 0;
    }
    if (x == 1) {
      return 1;
    }
    return yForT(tForX(x));
  };
}

function stepEnd(nsteps) {
  return x => Math.floor(x * nsteps) / nsteps;
}

function stepStart(nsteps) {
  return x => {
    const result = Math.floor(x * nsteps + 1.0) / nsteps;
    return (result > 1.0) ? 1.0 : result;
  };
}

function waitForAnimationFrames(frameCount) {
  return new Promise(resolve => {
    function handleFrame() {
      if (--frameCount <= 0) {
        resolve();
      } else {
        window.requestAnimationFrame(handleFrame); // wait another frame
      }
    }
    window.requestAnimationFrame(handleFrame);
  });
}

// Continually calls requestAnimationFrame until |minDelay| has elapsed
// as recorded using document.timeline.currentTime (i.e. frame time not
// wall-clock time).
function waitForAnimationFramesWithDelay(minDelay) {
  const startTime = document.timeline.currentTime;
  return new Promise(resolve => {
    (function handleFrame() {
      if (document.timeline.currentTime - startTime >= minDelay) {
        resolve();
      } else {
        window.requestAnimationFrame(handleFrame);
      }
    }());
  });
}

function runAndWaitForFrameUpdate(callback) {
  return new Promise(resolve => {
    window.requestAnimationFrame(() => {
      callback();
      window.requestAnimationFrame(resolve);
    });
  });
}

// Waits for a requestAnimationFrame callback in the next refresh driver tick.
function waitForNextFrame() {
  const timeAtStart = document.timeline.currentTime;
  return new Promise(resolve => {
   (function handleFrame() {
    if (timeAtStart === document.timeline.currentTime) {
      window.requestAnimationFrame(handleFrame);
    } else {
      resolve();
    }
  }());
  });
}

async function insertFrameAndAwaitLoad(test, iframe, doc) {
  const eventWatcher = new EventWatcher(test, iframe, ['load']);
  const event_promise = eventWatcher.wait_for('load');

  doc.body.appendChild(iframe);
  test.add_cleanup(() => { doc.body.removeChild(iframe); });

  await event_promise;
}

// Returns 'matrix()' or 'matrix3d()' function string generated from an array.
function createMatrixFromArray(array) {
  return (array.length == 16 ? 'matrix3d' : 'matrix') + `(${array.join()})`;
}

// Returns 'matrix3d()' function string equivalent to
// 'rotate3d(x, y, z, radian)'.
function rotate3dToMatrix3d(x, y, z, radian) {
  return createMatrixFromArray(rotate3dToMatrix(x, y, z, radian));
}

// Returns an array of the 4x4 matrix equivalent to 'rotate3d(x, y, z, radian)'.
// https://drafts.csswg.org/css-transforms-2/#Rotate3dDefined
function rotate3dToMatrix(x, y, z, radian) {
  const sc = Math.sin(radian / 2) * Math.cos(radian / 2);
  const sq = Math.sin(radian / 2) * Math.sin(radian / 2);

  // Normalize the vector.
  const length = Math.sqrt(x*x + y*y + z*z);
  x /= length;
  y /= length;
  z /= length;

  return [
    1 - 2 * (y*y + z*z) * sq,
    2 * (x * y * sq + z * sc),
    2 * (x * z * sq - y * sc),
    0,
    2 * (x * y * sq - z * sc),
    1 - 2 * (x*x + z*z) * sq,
    2 * (y * z * sq + x * sc),
    0,
    2 * (x * z * sq + y * sc),
    2 * (y * z * sq - x * sc),
    1 - 2 * (x*x + y*y) * sq,
    0,
    0,
    0,
    0,
    1
  ];
}

// Compare matrix string like 'matrix(1, 0, 0, 1, 100, 0)' with tolerances.
function assert_matrix_equals(actual, expected, description) {
  const matrixRegExp = /^matrix(?:3d)*\((.+)\)/;
  assert_regexp_match(actual, matrixRegExp,
    'Actual value is not a matrix')
  assert_regexp_match(expected, matrixRegExp,
    'Expected value is not a matrix');

  const actualMatrixArray =
    actual.match(matrixRegExp)[1].split(',').map(Number);
  const expectedMatrixArray =
    expected.match(matrixRegExp)[1].split(',').map(Number);

  assert_equals(actualMatrixArray.length, expectedMatrixArray.length,
    `dimension of the matrix: ${description}`);
  for (let i = 0; i < actualMatrixArray.length; i++) {
    assert_approx_equals(actualMatrixArray[i], expectedMatrixArray[i], 0.0001,
      `expected ${expected} but got ${actual}: ${description}`);
  }
}

// Compare rotate3d vector like '0 1 0 45deg' with tolerances.
function assert_rotate3d_equals(actual, expected, description) {
  const rotationRegExp =/^((([+-]?\d+(\.+\d+)?\s){3})?\d+(\.+\d+)?)deg/;

  assert_regexp_match(actual, rotationRegExp,
    'Actual value is not a rotate3d vector')
  assert_regexp_match(expected, rotationRegExp,
    'Expected value is not a rotate3d vector');

  const actualRotationVector =
    actual.match(rotationRegExp)[1].split(' ').map(Number);
  const expectedRotationVector =
    expected.match(rotationRegExp)[1].split(' ').map(Number);

  assert_equals(actualRotationVector.length, expectedRotationVector.length,
                `dimension of the matrix: ${description}`);
  for (let i = 0; i < actualRotationVector.length; i++) {
    assert_approx_equals(
        actualRotationVector[i],
        expectedRotationVector[i],
        0.0001,
        `expected ${expected} but got ${actual}: ${description}`);
  }
}

function assert_phase_at_time(animation, phase, currentTime) {
  animation.currentTime = currentTime;
  assert_phase(animation, phase);
}

function assert_phase(animation, phase) {
  const fillMode = animation.effect.getTiming().fill;
  const currentTime = animation.currentTime;

  if (phase === 'active') {
    // If the fill mode is 'none', then progress will only be non-null if we
    // are in the active phase, except for progress-based timelines where
    // currentTime = 100% is still 'active'.
    animation.effect.updateTiming({ fill: 'none' });
    if ('ScrollTimeline' in window && animation.timeline instanceof ScrollTimeline) {
        const isActive = animation.currentTime?.toString() == "100%" ||
                         animation.effect.getComputedTiming().progress != null;
        assert_true(isActive,
                    'Animation effect is in active phase when current time ' +
                    `is ${currentTime}.`);
    } else {
      assert_not_equals(animation.effect.getComputedTiming().progress, null,
                        'Animation effect is in active phase when current time ' +
                        `is ${currentTime}.`);
    }
  } else {
    // The easiest way to distinguish between the 'before' phase and the 'after'
    // phase is to toggle the fill mode. For example, if the progress is null
    // when the fill mode is 'none' but non-null when the fill mode is
    // 'backwards' then we are in the before phase.
    animation.effect.updateTiming({ fill: 'none' });
    assert_equals(animation.effect.getComputedTiming().progress, null,
                  `Animation effect is in ${phase} phase when current time ` +
                  `is ${currentTime} (progress is null with 'none' fill mode)`);

    animation.effect.updateTiming({
      fill: phase === 'before' ? 'backwards' : 'forwards',
    });
    assert_not_equals(animation.effect.getComputedTiming().progress, null,
                      `Animation effect is in ${phase} phase when current ` +
                      `time is ${currentTime} (progress is non-null with ` +
                      `appropriate fill mode)`);
  }

  // Reset fill mode to avoid side-effects.
  animation.effect.updateTiming({ fill: fillMode });
}


// Use with reftest-wait to wait until compositor commits are no longer deferred
// before taking the screenshot.
// crbug.com/1378671
async function waitForCompositorReady() {
  const animation =
      document.body.animate({ opacity: [ 0, 1 ] }, {duration: 1 });
  return animation.finished;
}

async function takeScreenshotOnAnimationsReady() {
  await Promise.all(document.getAnimations().map(a => a.ready));
  requestAnimationFrame(() => requestAnimationFrame(takeScreenshot));
}

