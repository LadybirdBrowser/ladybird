async_test(t => {
  function checkEntryList(entries) {
    assert_equals(entries.length, 1, "Only one navigation timing entry");
    assert_equals(entries[0].entryType, "navigation", "entryType is \"navigation\"");
    assert_equals(entries[0].name, window.location.toString(), "name is the address of the document");
  }
  // First observer creates second in callback to ensure the entry has been dispatched by the time
  // the second observer begins observing.
  new PerformanceObserver(t.step_func(entryList => {
    checkEntryList(entryList.getEntries());
    // Second observer requires 'buffered: true' to see the navigation entry.
    new PerformanceObserver(t.step_func_done(list => {
      checkEntryList(list.getEntries());
    })).observe({type: 'navigation', buffered: true});
  })).observe({entryTypes: ["navigation"]});
}, "PerformanceObserver with buffered flag sees previous navigation entry.");
