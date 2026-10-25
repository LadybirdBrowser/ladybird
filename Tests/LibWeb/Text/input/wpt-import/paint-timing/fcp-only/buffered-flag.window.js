setup({"hide_test_state": true});
async_test(t => {
  assert_implements(window.PerformancePaintTiming, "Paint Timing isn't supported.");
  // First observer creates a second one in the callback to ensure the entry has been dispatched
  // by the time the second observer begins observing.
  new PerformanceObserver(entries => {
    const entry_seen = entries.getEntriesByName('first-contentful-paint').length > 0;
    // Abort if we have not yet received the entry.
    if (!entry_seen)
      return;

    // Second observer requires 'buffered: true' to see the entry.
    new PerformanceObserver(t.step_func_done(list => {
        const fcp = list.getEntriesByName('first-contentful-paint');
        assert_equals(fcp.length, 1, 'Should have an fcp entry');
        const entry = fcp[0];
        assert_equals(entry.entryType, 'paint');
      })).observe({'type': 'paint', buffered: true});
  }).observe({'entryTypes': ['paint']});
  // Trigger the first contentful paint entry.
  const img = document.createElement("img");
  img.src = "../resources/circles.png";
  document.body.appendChild(img);
}, "PerformanceObserver with buffered flag sees previous FCP entry.");
