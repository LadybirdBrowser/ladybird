function createToggleEventSourceTest({
    description,
    target,
    openFunc,
    closeFunc,
    openSource,
    closeSource,
    skipBeforetoggle}) {
  promise_test(async () => {
    let beforetoggleEvent = null;
    let beforetoggleDuplicate = false;
    let toggleEvent = null;
    let toggleDuplicate = false;
    target.addEventListener('beforetoggle', event => {
      if (beforetoggleEvent) {
        beforetoggleDuplicate = true;
      }
      beforetoggleEvent = event;
    });
    target.addEventListener('toggle', event => {
      if (toggleEvent) {
        toggleDuplicate = true;
      }
      toggleEvent = event;
    });

    await openFunc();
    await new Promise(requestAnimationFrame);
    await new Promise(requestAnimationFrame);
    if (!skipBeforetoggle) {
      assert_true(!!beforetoggleEvent,
        'An opening beforetoggle event should have been fired.');
      assert_false(beforetoggleDuplicate,
        'Only one opening beforetoggle event should have been fired.');
      assert_equals(beforetoggleEvent.newState, 'open',
        'beforetoggle newState should be open.');
      assert_equals(beforetoggleEvent.source, openSource,
        'Opening beforetoggle.source.');
    }
    assert_true(!!toggleEvent,
      'An opening toggle event should have been fired.');
    assert_false(toggleDuplicate,
      'Only one opening toggle event should have been fired.');
    assert_equals(toggleEvent.newState, 'open',
      'toggle newstate should be open.');
    assert_equals(toggleEvent.source, openSource,
      'Opening toggle.source.');
    beforetoggleEvent = null;
    beforetoggleDuplicate = false;
    toggleEvent = null;
    toggleDuplicate = false;

    await closeFunc();
    await new Promise(requestAnimationFrame);
    await new Promise(requestAnimationFrame);

    if (!skipBeforetoggle) {
      assert_true(!!beforetoggleEvent,
        'A closing beforetoggle event should have been fired.');
      assert_false(beforetoggleDuplicate,
        'Only one closing beforetoggle event should have been fired.');
      assert_equals(beforetoggleEvent.newState, 'closed',
        'beforetoggle newState should be closed.');
      assert_equals(beforetoggleEvent.source, closeSource,
        'Closing beforetoggle.source.');
    }
    assert_true(!!toggleEvent,
      'A closing toggle event should have been fired.');
    assert_false(toggleDuplicate,
      'Only one closing toggle event should have been fired.');
    assert_equals(toggleEvent.newState, 'closed',
      'toggle newstate should be closed.');
    assert_equals(toggleEvent.source, closeSource,
      'Closing toggle.source.');
  }, description);
}
