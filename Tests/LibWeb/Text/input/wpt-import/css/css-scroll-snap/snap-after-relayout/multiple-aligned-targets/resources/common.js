// Utility functions for scroll snap tests which verify User-Agents' snap point
// selection logic when multiple snap targets are aligned.
// It depends on methods in /resources/testdriver-actions.js and
// /dom/event/scrolling/scroll_support.js so html files using these functions
// should include those files as <script>s.

// This function should be used by scroll snap WPTs wanting to test snap target
// selection when scrolling to multiple aligned targets.
// It assumes scroll-snap-align: start alignment and tries to align to the lists
// of snap targets provided, |elements_x| and |elements_y|, which are all
// expected to be at the same offset in the relevant axis.
async function scrollToAlignedElements(scroller, elements_x, elements_y) {
  let target_offset_y = null;
  let target_offset_x = null;
  for (const e of elements_y) {
    if (target_offset_y != null) {
      assert_equals(e.offsetTop, target_offset_y,
        `${e.id} is at y offset ${target_offset_y}`);
    } else {
      target_offset_y = e.offsetTop;
    }
  }
  for (const e of elements_x) {
    if (target_offset_x != null) {
      assert_equals(e.offsetLeft, target_offset_x,
        `${e.id} is at x offset ${target_offset_x}`);
    } else {
      target_offset_x = e.offsetLeft;
    }
  }
  assert_true((target_offset_x != null) || (target_offset_y != null),
      "scrolls in at least 1 axis");

  if ((target_offset_x != null && scroller.scrollLeft != target_offset_x) ||
      (target_offset_y != null && scroller.scrollTop != target_offset_y)) {
    const scrollend_promise = waitForScrollendEventNoTimeout(scroller);
    const delta_x =
      (target_offset_x || scroller.scrollLeft) - scroller.scrollLeft;
    const delta_y =
      (target_offset_y || scroller.scrollTop) - scroller.scrollTop;
    scroller.scrollBy(delta_x, delta_y);
    await scrollend_promise;
  }
  if (target_offset_y) {
    assert_equals(scroller.scrollTop, target_offset_y, "vertical scroll done");
  }
  if (target_offset_x) {
    assert_equals(scroller.scrollLeft, target_offset_x, "horizontal scroll done");
  }
}

// This function verifies the snap target that a scroller picked by triggering
// a layout change and observing which target is followed. Tests using this
// method should ensure that there is at least 100px of room to scroll in the
// desired axis.
// It assumes scroll-snap-align: start alignment.
function verifySelectedSnapTarget(t, scroller, expected_snap_target, axis) {
  // Save initial style.
  const initial_left = getComputedStyle(expected_snap_target).left;
  const initial_top = getComputedStyle(expected_snap_target).top;
  if (axis == "y") {
    // Move the expected snap target along the y axis.
    const initial_scroll_top = scroller.scrollTop;
    const target_top = expected_snap_target.offsetTop + 100;
    expected_snap_target.style.top = `${target_top}px`;
    // Wrap these asserts in t.step (which catches exceptions) so that even if
    // they fail, we'll get to undo the style changes we made, allowing
    // subsequent tests to run with the expected style/layout.
    t.step(() => {
      assert_equals(scroller.scrollTop, expected_snap_target.offsetTop,
        `scroller followed ${expected_snap_target.id} in y axis after layout change`);
      assert_not_equals(scroller.scrollTop, initial_scroll_top,
        "scroller actually scrolled in y axis");
    });
  } else {
    // Move the expected snap target along the x axis.
    const initial_scroll_left = scroller.scrollLeft;
    const target_left = expected_snap_target.offsetLeft + 100;
    expected_snap_target.style.left = `${target_left}px`;
    t.step(() => {
      assert_equals(scroller.scrollLeft, expected_snap_target.offsetLeft,
        `scroller followed ${expected_snap_target.id} in x axis after layout change`);
      assert_not_equals(scroller.scrollLeft, initial_scroll_left,
        "scroller actually scrolled in x axis");
    });
  }
  // Undo style changes.
  expected_snap_target.style.top = initial_top;
  expected_snap_target.style.left = initial_left;
}

// This is a utility function for tests which verify that the correct element
// is snapped to when snapping at the end of a scroll.
async function runScrollSnapSelectionVerificationTest(t, scroller,
    aligned_elements_x=[], aligned_elements_y=[], axis="",
    expected_target_x=null, expected_target_y=null) {
  // Save initial scroll offset.
  const initial_scroll_left = scroller.scrollLeft;
  const initial_scroll_top = scroller.scrollTop;
  await scrollToAlignedElements(scroller, aligned_elements_x,
    aligned_elements_y);
  t.step(() => {
    if (axis == "y" || axis == "both") {
      verifySelectedSnapTarget(t, scroller, expected_target_y, "y");
    }
    if (axis == "x" || axis == "both") {
      verifySelectedSnapTarget(t, scroller, expected_target_x, "x");
    }
  });
  // Restore initial scroll offsets.
  await waitForScrollReset(t, scroller, initial_scroll_left, initial_scroll_top);
}

// This is a utility function for tests verifying that a layout shift does not
// cause a scroller to change its selected snap target.
// It assumes the element to be aligned have scroll-snap-align: start.
// It tries to align the list of snap targets provided, |elements| with the
// current snap target.
function shiftLayoutToAlignElements(elements, target, axis) {
  for (let element of elements) {
    if (axis == "y") {
      element.style.top = `${target.offsetTop}px`;
    } else {
      element.style.left = `${target.offsetLeft}px`;
    }
  }
}

// This is a utility function for tests verifying that a layout shift does not
// cause a scroller to change its selected snap target.
// It assumes scroll-snap-align: start alignment.
async function runLayoutSnapSeletionVerificationTest(t, scroller, elements_to_align,
  expected_target, axis) {
  // Save initial scroll offsets and position.
  const initial_scroll_left = scroller.scrollLeft;
  const initial_scroll_top = scroller.scrollTop;
  let initial_tops = [];
  for (const element of elements_to_align) {
    initial_tops.push(getComputedStyle(element).top);
  }

  shiftLayoutToAlignElements(elements_to_align, expected_target, axis);
  verifySelectedSnapTarget(t, scroller, expected_target, axis);

  // Restore initial scroll offset and position states.
  let num_elements = initial_tops.length;
  for (let i = 0; i < num_elements; i++) {
    elements_to_align[i].style.top = initial_tops[i];
  }
  // Restore initial scroll offsets.
  const scrollend_promise = new Promise((resolve) => {
    scroller.addEventListener("scrollend", resolve);
  });
  scroller.scrollTo(initial_scroll_left, initial_scroll_top);
  await scrollend_promise;
}

function focusAndAssert(element, preventScroll=false) {
  element.focus({preventScroll: preventScroll});
  assert_equals(document.activeElement, element);
}
