function waitForRender() {
  return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
}

async function clickOn(element, useTouch) {
  await waitForRender();
  let rect = element.getBoundingClientRect();
  let actions = new test_driver.Actions();
  // FIXME: Switch to pointerMove(0, 0, {origin: element}) once
  // https://github.com/web-platform-tests/wpt/issues/41257 is fixed.
  if (useTouch) {
    actions.addPointer('touch1', 'touch');
  }
  await actions
      .pointerMove(Math.round(rect.x + rect.width / 2), Math.round(rect.y + rect.height / 2), {})
      .pointerDown({button: actions.ButtonType.LEFT})
      .pointerUp({button: actions.ButtonType.LEFT})
      .send();
  await waitForRender();
}
async function sendTab() {
  await waitForRender();
  const kTab = '\uE004';
  await test_driver.send_keys(document.activeElement || document.documentElement, kTab);
  await waitForRender();
}
async function sendShiftTab() {
  await waitForRender();
  const kShift = '\uE008';
  const kTab = '\uE004';
  await new test_driver.Actions()
    .keyDown(kShift)
    .keyDown(kTab)
    .keyUp(kTab)
    .keyUp(kShift)
    .send();
  await waitForRender();
}
async function sendEscape() {
  await waitForRender();
  await test_driver.send_keys(document.activeElement || document.documentElement,'\uE00C'); // Escape
  await waitForRender();
}
async function sendEnter() {
  await waitForRender();
  await test_driver.send_keys(document.activeElement || document.documentElement,'\uE007'); // Enter
  await waitForRender();
}
function isElementVisible(el) {
  return !!(el.offsetWidth || el.offsetHeight || el.getClientRects().length);
}
async function finishAnimations(popover) {
  popover.getAnimations({subtree: true}).forEach(animation => animation.finish());
  await waitForRender();
}

// This is a "polyfill" of sorts for the `defaultopen` attribute.
// It can be called before window.load is complete, and it will
// show defaultopen popovers according to the rules previously part
// of the popover API: any popover=manual popover can be shown this
// way, and only the first popover=auto popover.
function showDefaultopenPopoversOnLoad() {
  function show() {
    const popovers = Array.from(document.querySelectorAll('[popover][defaultopen]'));
    popovers.forEach((p) => {
        // The showPopover calls below aren't guarded by a check on the popover
        // open/closed status. If they throw exceptions, this function was
        // probably called at a bad time. However, a check is made for open
        // <dialog open> elements.
        if (p instanceof HTMLDialogElement && p.hasAttribute('open'))
          return;
        switch (p.popover) {
          case 'auto':
            if (!document.querySelector('[popover]:popover-open'))
              p.showPopover();
            return;
          case 'manual':
            p.showPopover();
            return;
          default:
            assert_unreached(`Unknown popover type ${p.popover}`);
        }
      });
  }
  if (document.readyState === 'complete') {
    show();
  } else {
    window.addEventListener('load',show,{once:true});
  }
}

function assertPopoverVisibility(popover, isPopover, expectedVisibility, message) {
  const isVisible = isElementVisible(popover);
  assert_equals(isVisible, expectedVisibility,`${message}: Expected this element to be ${expectedVisibility ? "visible" : "not visible"}`);
  // Check other things related to being visible or not:
  if (isVisible) {
    assert_not_equals(window.getComputedStyle(popover).display,'none');
    assert_equals(popover.matches(':popover-open'),isPopover,`${message}: Visible popovers should match :popover-open`);
  } else {
    assert_equals(window.getComputedStyle(popover).display,'none',`${message}: Non-showing popovers should have display:none`);
    assert_false(popover.matches(':popover-open'),`${message}: Non-showing popovers should *not* match :popover-open`);
  }
}

function assertIsFunctionalPopover(popover, checkVisibility) {
  assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/false, 'A popover should start out hidden');
  popover.showPopover();
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/true, 'After showPopover(), a popover should be visible');
  popover.showPopover(); // Calling showPopover on a showing popover should not throw.
  popover.hidePopover();
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/false, 'After hidePopover(), a popover should be hidden');
  popover.hidePopover(); // Calling hidePopover on a hidden popover should not throw.
  popover.togglePopover();
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/true, 'After togglePopover() on hidden popover, it should be visible');
  popover.togglePopover();
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/false, 'After togglePopover() on visible popover, it should be hidden');
  popover.togglePopover(/*force=*/true);
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/true, 'After togglePopover(true) on hidden popover, it should be visible');
  popover.togglePopover(/*force=*/true);
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/true, 'After togglePopover(true) on visible popover, it should be visible');
  popover.togglePopover(/*force=*/false);
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/false, 'After togglePopover(false) on visible popover, it should be hidden');
  popover.togglePopover(/*force=*/false);
  if (checkVisibility) assertPopoverVisibility(popover, /*isPopover*/true, /*expectedVisibility*/false, 'After togglePopover(false) on hidden popover, it should be hidden');
  const parent = popover.parentElement;
  popover.remove();
  assert_throws_dom("InvalidStateError",() => popover.showPopover(),'Calling showPopover on a disconnected popover should throw InvalidStateError');
  popover.hidePopover(); // Calling hidePopover on a disconnected popover should not throw.
  assert_throws_dom("InvalidStateError",() => popover.togglePopover(),'Calling hidePopover on a disconnected popover should throw InvalidStateError');
  parent.appendChild(popover);
}

function assertNotAPopover(nonPopover) {
  // If the non-popover element nonetheless has a 'popover' attribute, it should
  // be invisible. Otherwise, it should be visible.
  const expectVisible = !nonPopover.hasAttribute('popover');
  assertPopoverVisibility(nonPopover, /*isPopover*/false, expectVisible, 'A non-popover should start out visible');
  assert_throws_dom("NotSupportedError",() => nonPopover.showPopover(),'Calling showPopover on a non-popover should throw NotSupported');
  assertPopoverVisibility(nonPopover, /*isPopover*/false, expectVisible, 'Calling showPopover on a non-popover should leave it visible');
  assert_throws_dom("NotSupportedError",() => nonPopover.hidePopover(),'Calling hidePopover on a non-popover should throw NotSupported');
  assertPopoverVisibility(nonPopover, /*isPopover*/false, expectVisible, 'Calling hidePopover on a non-popover should leave it visible');
  assert_throws_dom("NotSupportedError",() => nonPopover.togglePopover(),'Calling togglePopover on a non-popover should throw NotSupported');
  assertPopoverVisibility(nonPopover, /*isPopover*/false, expectVisible, 'Calling togglePopover on a non-popover should leave it visible');
}

function shadowIncludingActiveElement() {
  let element = document.activeElement;
  while (element.shadowRoot?.activeElement) {
    element = element.shadowRoot.activeElement;
  }
  return element;
}

async function verifyFocusOrder(order,description) {
  order[0].focus();
  for(let i=0;i<order.length;++i) {
    // Press tab between each check, excluding first (because it should already be focused)
    // and the last (because tabbing after the last element may send focus into browser chrome).
    if (i != 0) {
      await sendTab();
    }
    const control = order[i];
    assert_equals(shadowIncludingActiveElement(),control,`${description}: Step ${i+1}`);
  }
  for(let i=order.length-1;i>=0;--i) {
    const control = order[i];
    assert_equals(shadowIncludingActiveElement(),control,`${description}: Step ${i+1} (backwards)`);
    // Press shift+tab between each check, excluding last (because it should already be focused)
    // and the first (because shift+tabbing after the last element may send focus into browser chrome).
    if (i != 0) {
      await sendShiftTab();
    }
  }
}
