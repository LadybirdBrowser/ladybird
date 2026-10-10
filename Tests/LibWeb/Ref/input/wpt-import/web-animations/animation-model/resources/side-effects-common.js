'use strict';

const PROPERTY_OPACITY = 0;
const PROPERTY_TRANSFORM = 1;
const PROPERTY_BGCOLOR = 2;

const PHASE_BEFORE = 10;
const PHASE_ACTIVE = 11;
const PHASE_AFTER = 12;

const STATE_CURRENT = 100;
const STATE_IN_EFFECT = 101;
const STATE_NONE = 102;

// Creates an animation in the given state/page used to test side-effects. See:
// https://drafts.csswg.org/web-animations-1/#animation-effect-phases-and-states
//
// testcase - A query string for the test case root. Must have a descendant
//            with the 'target' class that will be animated.
// state - One of the STATE_ constants above. Configures the animation to be
//         either "current", "in effect" or neither.
// property - One of the PROPERTY_ constants above, the property the animation
//            will target.
// phase - One of the PHASE_ constants above. Configures the animation to be in
//         the before/active/after phase.
function setupAnimation(testcase, state, property, phase) {
  const root = document.querySelector(testcase);
  const effect_target = root.querySelector('.target');

  let keyframe;
  if (property == PROPERTY_OPACITY)
    keyframe = { opacity: 1};
  else if (property == PROPERTY_TRANSFORM)
    keyframe = { transform: 'translateX(0px)' };
  else if (property == PROPERTY_BGCOLOR)
    keyframe = { backgroundColor: 'red' };
  else
    throw new Error('Unexpected property');

  const kPhaseDuration = 1000000;
  const kBeforePhaseTime = kPhaseDuration / 2;
  const kActivePhaseTime = kPhaseDuration + kPhaseDuration / 2;
  const kAfterPhaseTime = 2 * kPhaseDuration + kPhaseDuration / 2;

  const options = {
    duration: kPhaseDuration,
    delay: kPhaseDuration,
    endDelay: kPhaseDuration,

    easing: 'steps(1, jump-both)',

    fill: (state == STATE_IN_EFFECT ? 'both' : 'none'),
  };

  const animation = effect_target.animate(
    [ keyframe, keyframe ], options);

  switch(phase) {
    case PHASE_BEFORE:
      animation.currentTime = kBeforePhaseTime;
      if (state == STATE_IN_EFFECT || state == STATE_NONE)
        animation.playbackRate = -1;
      break;

    case PHASE_ACTIVE:
      if (state == STATE_NONE)
        throw new Error("Cannot have state[NONE] in the active phase");

      animation.currentTime = kActivePhaseTime;
      break;

    case PHASE_AFTER:
      animation.currentTime = kAfterPhaseTime;
      if (state == STATE_CURRENT)
        animation.playbackRate = -1;
      break;

    default:
      throw new Error('Unexpected phase');
  }
}
