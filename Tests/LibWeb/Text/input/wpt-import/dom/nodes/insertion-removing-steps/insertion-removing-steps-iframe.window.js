// These tests ensure that:
//   1. The HTML element insertion steps for iframes [1] run *after* all DOM
//      insertion mutations associated with any given call to
//      #concept-node-insert [2] (which may insert many elements at once).
//      Consequently, a preceding element's insertion steps can observe the
//      side-effects of later elements being connected to the DOM, but cannot
//      observe the side-effects of the later element's own insertion steps [1],
//      since insertion steps are run in order after all DOM insertion mutations
//      are complete.
//   2. The HTML element removing steps for iframes [3] *do not* synchronously
//      run script during child navigable destruction. Therefore, script cannot
//      observe the state of the DOM in the middle of iframe removal, even when
//      multiple iframes are being removed in the same task. Iframe removal,
//      from the perspective of the parent's DOM tree, is atomic.
//
// [1]: https://html.spec.whatwg.org/C#the-iframe-element:html-element-insertion-steps
// [2]: https://dom.spec.whatwg.org/#concept-node-insert
// [3]: https://html.spec.whatwg.org/C#the-iframe-element:html-element-removing-steps

promise_test(async t => {
  const fragment = new DocumentFragment();

  const iframe1 = fragment.appendChild(document.createElement('iframe'));
  const iframe2 = fragment.appendChild(document.createElement('iframe'));

  t.add_cleanup(() => {
    iframe1.remove();
    iframe2.remove();
  });

  let iframe1Loaded = false, iframe2Loaded = false;
  iframe1.onload = t.step_func(e => {
    // iframe1 assertions:
    iframe1Loaded = true;
    assert_equals(window.frames.length, 1,
        "iframe1 load event can observe its own participation in the frame " +
        "tree");
    assert_equals(iframe1.contentWindow, window.frames[0]);

    // iframe2 assertions:
    assert_false(iframe2Loaded,
        "iframe2's load event hasn't fired before iframe1's");
    assert_true(iframe2.isConnected,
        "iframe1 can observe that iframe2 is connected to the DOM...");
    assert_equals(iframe2.contentWindow, null,
        "... but iframe1 cannot observe iframe2's contentWindow because " +
        "iframe2's insertion steps have not been run yet");
  });

  iframe2.onload = t.step_func(e => {
    iframe2Loaded = true;
    assert_equals(window.frames.length, 2,
        "iframe2 load event can observe its own participation in the frame tree");
    assert_equals(iframe1.contentWindow, window.frames[0]);
    assert_equals(iframe2.contentWindow, window.frames[1]);
  });

  // Synchronously consecutively adds both `iframe1` and `iframe2` to the DOM,
  // invoking their insertion steps (and thus firing each of their `load`
  // events) in order. `iframe1` will be able to observe itself in the DOM but
  // not `iframe2`, and `iframe2` will be able to observe both itself and
  // `iframe1`.
  document.body.append(fragment);
  assert_true(iframe1Loaded, "iframe1 loaded");
  assert_true(iframe2Loaded, "iframe2 loaded");
}, "Insertion steps: load event fires synchronously *after* iframe DOM " +
   "insertion, as part of the iframe element's insertion steps");

// There are several versions of the removal variant, since there are several
// ways to remove multiple elements "at once". For example:
//   1. `node.innerHTML = ''` ultimately runs
//      https://dom.spec.whatwg.org/#concept-node-replace-all which removes all
//      of a node's children.
//   2. `node.replaceChildren()` which follows roughly the same path above.
//   3. `node.remove()` on a parent of many children will invoke not the DOM
//      remove algorithm, but rather the "removing steps" hook [1], for each
//      child.
//
// [1]: https://dom.spec.whatwg.org/#concept-node-remove-ext

function runRemovalTest(removal_method) {
  promise_test(async t => {
    const div = document.createElement('div');

    const iframe1 = div.appendChild(document.createElement('iframe'));
    const iframe2 = div.appendChild(document.createElement('iframe'));
    document.body.append(div);

    // Now that both iframes have been inserted into the DOM, we'll set up a
    // MutationObserver that we'll use to ensure that multiple synchronous
    // mutations (removals) are only observed atomically at the end. Specifically,
    // the observer's callback is not invoked synchronously for each removal.
    let observerCallbackInvoked = false;
    const removalObserver = new MutationObserver(t.step_func(mutations => {
      assert_false(observerCallbackInvoked,
          "MO callback is only invoked once, not multiple times, i.e., for " +
          "each removal");
      observerCallbackInvoked = true;
      assert_equals(mutations.length, 1, "Exactly one MutationRecord is recorded");
      assert_equals(mutations[0].removedNodes.length, 2);
      assert_equals(window.frames.length, 0,
          "No iframe Windows exist when the MO callback is run");
      assert_equals(document.querySelector('iframe'), null,
          "No iframe elements are connected to the DOM when the MO callback is " +
          "run");
    }));

    removalObserver.observe(div, {childList: true});
    t.add_cleanup(() => removalObserver.disconnect());

    let iframe1UnloadFired = false, iframe2UnloadFired = false;
    let iframe1PagehideFired = false, iframe2PagehideFired = false;
    iframe1.contentWindow.addEventListener('pagehide', e => {
      assert_false(iframe1UnloadFired, "iframe1 pagehide fires before unload");
      iframe1PagehideFired = true;
    });
    iframe2.contentWindow.addEventListener('pagehide', e => {
      assert_false(iframe2UnloadFired, "iframe2 pagehide fires before unload");
      iframe2PagehideFired = true;
    });
    iframe1.contentWindow.addEventListener('unload', e => iframe1UnloadFired = true);
    iframe2.contentWindow.addEventListener('unload', e => iframe2UnloadFired = true);

    // Each `removal_method` will trigger the synchronous removal of each of
    // `div`'s (iframe) children. This will synchronously, consecutively
    // invoke HTML's "destroy a child navigable" (per [1]), for each iframe.
    //
    // [1]: https://html.spec.whatwg.org/C#the-iframe-element:destroy-a-child-navigable

    if (removal_method === 'replaceChildren') {
      div.replaceChildren();
    } else if (removal_method === 'remove') {
      div.remove();
    } else if (removal_method === 'innerHTML') {
      div.innerHTML = '';
    }

    assert_false(iframe1PagehideFired, "iframe1 pagehide did not fire");
    assert_false(iframe2PagehideFired, "iframe2 pagehide did not fire");
    assert_false(iframe1UnloadFired, "iframe1 unload did not fire");
    assert_false(iframe2UnloadFired, "iframe2 unload did not fire");

    assert_false(observerCallbackInvoked,
        "MO callback is not invoked synchronously after removals");

    // Wait one microtask.
    await Promise.resolve();

    if (removal_method !== 'remove') {
      assert_true(observerCallbackInvoked,
          "MO callback is invoked asynchronously after removals");
    }
  }, `Removing steps (${removal_method}): script does not run synchronously during iframe destruction`);
}

runRemovalTest('innerHTML');
runRemovalTest('replaceChildren');
runRemovalTest('remove');
