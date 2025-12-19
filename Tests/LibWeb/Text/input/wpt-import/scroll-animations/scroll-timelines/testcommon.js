'use strict';

// Builds a generic structure that looks like:
//
// <div class="scroller">  // 100x100 viewport
//   <div class="contents"></div>  // 500x500
// </div>
//
// The |scrollerOverrides| and |contentOverrides| parameters are maps which
// are applied to the scroller and contents style after basic setup.
//
// Appends the outer 'scroller' element to the document body, and returns it.
function setupScrollTimelineTest(
    scrollerOverrides = new Map(), contentOverrides = new Map()) {
  let scroller = document.createElement('div');
  scroller.style.width = '100px';
  scroller.style.height = '100px';
  // Hide the scrollbars, but maintain the ability to scroll. This setting
  // ensures that variability in scrollbar sizing does not contribute to test
  // failures or flakes.
  scroller.style.overflow = 'hidden';
  for (const [key, value] of scrollerOverrides) {
    scroller.style[key] = value;
  }

  let contents = document.createElement('div');
  contents.style.width = '500px';
  contents.style.height = '500px';
  for (const [key, value] of contentOverrides) {
    contents.style[key] = value;
  }

  scroller.appendChild(contents);
  document.body.appendChild(scroller);
  return scroller;
}

// Helper method to calculate the current time, implementing only step 5 of
// https://wicg.github.io/scroll-animations/#current-time-algorithm
function calculateCurrentTime(
    currentScrollOffset, startScrollOffset, endScrollOffset) {
  return ((currentScrollOffset - startScrollOffset) /
          (endScrollOffset - startScrollOffset)) *
         100;
}

function createScroller(test) {
  var scroller = createDiv(test);
  scroller.innerHTML = "<div class='contents'></div>";
  scroller.classList.add('scroller');
  // Trigger layout run.
  scroller.scrollTop;
  return scroller;
}

function createScrollerWithStartAndEnd(test, orientationClass = 'vertical') {
  var scroller = createDiv(test);
  scroller.innerHTML =
    `<div class='contents'>
        <div id='start'></div>
        <div id='end'></div>
      </div>`;
  scroller.classList.add('scroller');
  scroller.classList.add(orientationClass);

  return scroller;
}

function createScrollTimeline(test, options) {
  options = options || {
    source: createScroller(test)
  }
  return new ScrollTimeline(options);
}

function createScrollLinkedAnimation(test, timeline) {
  return createScrollLinkedAnimationWithTiming(test, /* duration in ms*/ 1000, timeline);
}

function createScrollLinkedAnimationWithTiming(test, timing, timeline) {
  if (timeline === undefined)
    timeline = createScrollTimeline(test);
  const KEYFRAMES = { opacity: [0, 1] };
  return new Animation(
    new KeyframeEffect(createDiv(test), KEYFRAMES, timing), timeline);
}

function createViewTimeline(t) {
  const parent = document.querySelector('.scroller');
  const elem = document.createElement('div');
  elem.id = 'target';
  t.add_cleanup(() => {
    elem.remove();
  });
  parent.appendChild(elem);
  return new ViewTimeline({ subject: elem });
}

function createAnimation(t) {
  const elem = createDiv(t);
  const animation = elem.animate({ opacity: [1, 0] }, 1000);
  return animation;
}

function assert_approx_equals_or_null(actual, expected, tolerance, name) {
  if (actual === null || expected === null){
    assert_equals(actual, expected, name);
  }
  else {
    assert_approx_equals(actual, expected, tolerance, name);
  }
}

function assert_percents_approx_equal(actual, expected, maxScroll,
                                      description) {
  // Base the tolerance on being out by up to half a pixel.
  const tolerance = 0.5 / maxScroll * 100;
  assert_equals(actual.unit, "percent", `'actual' unit type must be ` +
      `'percent' for "${description}"`);
  assert_true(actual instanceof CSSUnitValue, `'actual' must be of type ` +
      `CSSNumberish for "${description}"`);
  if (expected instanceof CSSUnitValue){
    // Verify that when the expected in a CSSUnitValue, it is the correct unit
    // type
    assert_equals(expected.unit, "percent", `'expected' unit type must be ` +
        `'percent' for "${description}"`);
    assert_approx_equals(actual.value, expected.value, tolerance,
        `values do not match for "${description}"`);
  } else if (typeof expected, "number"){
    assert_approx_equals(actual.value, expected, tolerance,
        `values do not match for "${description}"`);
  }
}

function assert_percents_equal(actual, expected, description) {
  // Rough estimate of the default size of the scrollable area based on
  // sizes in setupScrollTimelineTest.
  const defaultScrollRange = 400;
  return assert_percents_approx_equal(actual, expected, defaultScrollRange,
                                      description);
}
