// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

async function reset(t, scrollers) {
  for (const scroller of scrollers) {
    await waitForScrollReset(t, scroller);
  }
}

/**
 * This tests executing scrollIntoView on multiple scroll containers at the same
 * time. It assumes and verifies vertical scrolling.
 */
async function simultaneousScrollIntoViewsTest(test,
                                               behaviors,
                                               targets,
                                               scrollers,
                                               target_offsets) {
  assert_equals(targets.length, behaviors.length,
    "equal numbers of targets and behaviors provided");
  assert_equals(scrollers.length, target_offsets.length,
    "equal numbers of scrollers and target_offsets provided");
  await reset(test, scrollers);
  await waitForCompositorCommit();

  // All scrollers should be at an offset of 0.
  for (const scroller of scrollers) {
    assert_equals(scroller.scrollTop, 0, `${scroller.id}'s scrollTop is reset`);
  }

  const scrollend_promises = Array.from(scrollers, (scroller) => {
    return waitForScrollEndFallbackToDelayWithoutScrollEvent(scroller);
  });

  // Scroll all targets into view.
  for (let idx = 0; idx < targets.length; idx++) {
    targets[idx].scrollIntoView({
      block: "start",
      behavior: behaviors[idx]
    });
  }
  await Promise.all(scrollend_promises);

  // Verify the expected positions os all scrollers.
  for (let idx = 0; idx < scrollers.length; idx++) {
    assert_approx_equals(scrollers[idx].scrollTop, target_offsets[idx], 1,
      `scrollIntoView finished executing on ${scrollers[idx].id}`
    );
  }
}
