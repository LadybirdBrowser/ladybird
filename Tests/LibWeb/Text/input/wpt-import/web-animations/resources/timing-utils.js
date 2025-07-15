'use strict';

// =======================================
//
// Utility functions for testing timing
//
// =======================================


// ------------------------------
//  Helper functions
// ------------------------------

// Utility function to check that a subset of timing properties have their
// default values.
function assert_default_timing_except(effect, propertiesToSkip) {
  const defaults = {
    delay: 0,
    endDelay: 0,
    fill: 'auto',
    iterationStart: 0,
    iterations: 1,
    duration: 'auto',
    direction: 'normal',
    easing: 'linear',
  };

  for (const prop of Object.keys(defaults)) {
    if (propertiesToSkip.includes(prop)) {
      continue;
    }

    assert_equals(
      effect.getTiming()[prop],
      defaults[prop],
      `${prop} parameter has default value:`
    );
  }
}

function waitForAnimationTime(animation, time) {
  return new Promise((resolve) => {
    function raf() {
      if (animation.currentTime < time) {
        requestAnimationFrame(raf);
      } else {
        resolve();
      }
    }
    requestAnimationFrame(raf);
  });
}
