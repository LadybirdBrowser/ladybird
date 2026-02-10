'use strict';

/**
 * Returns a Promise that is resolved after a CSS scroll timeline is created (as
 * the result of a style change) and a snapshot has been taken, so that the
 * animation style is correctly reflected by getComputedStyle().
 * Technically, this only takes a full frame update. We implement this as two
 * requestAnimationFrame callbacks because the result will be available at the
 * beginning of the second frame.
 */
async function waitForCSSScrollTimelineStyle() {
  await waitForNextFrame();
  await waitForNextFrame();
}

function assert_implements_animation_timeline() {
  assert_implements(CSS.supports('animation-timeline:--foo'),
      'animation-timeline not supported');
}
