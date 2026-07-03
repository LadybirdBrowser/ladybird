// Asserts that the anchored element is at the top/bottom/left/right of the
// anchor.
function assert_fallback_position(anchored, anchor, direction) {
  let anchoredRect = anchored.getBoundingClientRect();
  let anchorRect = anchor.getBoundingClientRect();
  let message = `Anchored element should be at the ${direction} of anchor`;
  switch (direction) {
    case 'top':
      assert_equals(anchoredRect.bottom, anchorRect.top, message);
      return;
    case 'bottom':
      assert_equals(anchoredRect.top, anchorRect.bottom, message);
      return;
    case 'left':
      assert_equals(anchoredRect.right, anchorRect.left, message);
      return;
    case 'right':
      assert_equals(anchoredRect.left, anchorRect.right, message);
      return;
    default:
      assert_unreached('unsupported direction');
  }
}

async function waitUntilNextAnimationFrame() {
  return new Promise(resolve => requestAnimationFrame(resolve));
}

// This function is a thin wrapper around `checkLayout` (from
// resources/check-layout-th.js) and simply reads the `CHECK_LAYOUT_DELAY`
// variable to optionally add a delay. This global variable is not intended
// to be set by other tests; instead, polyfills can set it to give themselves
// time to apply changes before proceeding with assertions about the layout.
// Tests that call this function and then do additional work after the call
// should `await` it to avoid race conditions.
window.checkLayoutForAnchorPos = async function(selectorList, callDone = true) {
  if (window.CHECK_LAYOUT_DELAY) {
    assert_equals(window.INJECTED_SCRIPT,undefined,'CHECK_LAYOUT_DELAY is only allowed when serving WPT with --injected-script.');
    await waitUntilNextAnimationFrame();
    await waitUntilNextAnimationFrame();
    await waitUntilNextAnimationFrame();
  }
  return window.checkLayout(selectorList, callDone);
}
