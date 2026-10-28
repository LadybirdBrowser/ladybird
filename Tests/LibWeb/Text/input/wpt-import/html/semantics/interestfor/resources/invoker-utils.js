function waitForRender() {
  return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
}
async function clickOn(element) {
  await waitForRender();
  let rect = element.getBoundingClientRect();
  let actions = new test_driver.Actions();
  // FIXME: Switch to pointerMove(0, 0, {origin: element}) once
  // https://github.com/web-platform-tests/wpt/issues/41257 is fixed.
  await actions
      .pointerMove(Math.round(rect.x + rect.width / 2), Math.round(rect.y + rect.height / 2), {})
      .pointerDown({button: actions.ButtonType.LEFT})
      .pointerUp({button: actions.ButtonType.LEFT})
      .send();
  await waitForRender();
}
async function focusOn(element) {
  element.focus();
  await waitForRender();
  assert_equals(document.activeElement,element,'focus should be on element');
}
async function hoverOver(element) {
  await waitForRender();
  let rect = element.getBoundingClientRect();
  let actions = new test_driver.Actions();
  // FIXME: Switch to pointerMove(0, 0, {origin: element}) once
  // https://github.com/web-platform-tests/wpt/issues/41257 is fixed.
  await actions
      .pointerMove(Math.round(rect.x + rect.width / 2), Math.round(rect.y + rect.height / 2), {})
      .send();
  await waitForRender();
}
async function longPress(element) {
  await waitForRender();
  let rect = element.getBoundingClientRect();
  // FIXME: Switch to pointerMove(0, 0, {origin: element}) once
  // https://github.com/web-platform-tests/wpt/issues/41257 is fixed.
  const x = Math.round(rect.x + rect.width / 2);
  const y = Math.round(rect.y + rect.height / 2);
  await new test_driver.Actions()
    .addPointer("touchPointer", "touch")
    .pointerMove(x, y, {sourceName: "touchPointer"})
    .pointerDown({sourceName: "touchPointer"})
    // This needs to be long enough to trigger long-press on all platforms:
    .pause(1000, "pointer", {sourceName: "touchPointer"})
    .pointerUp({sourceName: "touchPointer"})
    .send();
  await waitForRender();
}
function mouseOverAndRecord(t,element) {
  let timingInfo = {element, started: performance.now()};
  return (new test_driver.Actions())
      .pointerMove(0, 0, {origin: element})
      .send()
      .then(() => timingInfo);
}
function focusAndRecord(t,element) {
  let timingInfo = {element, started: performance.now()};
  element.focus();
  return timingInfo;
}
async function hoverOrFocus(invokerMethod,element) {
  if (invokerMethod === 'hover') {
    await hoverOver(element);
  } else {
    assert_equals(invokerMethod,'focus');
    element.focus();
    await waitForRender();
  }
}
async function mouseOverOrFocusAndRecord(t,invokerMethod,element) {
  if (invokerMethod === 'hover') {
    return await mouseOverAndRecord(t,element);
  } else {
    assert_equals(invokerMethod,'focus');
    return focusAndRecord(t,element);
  }
}
// Note that this may err on the side of being too large (reporting a number
// that is larger than the actual time since the mouseover happened), due to how
// `timingInfo.started` is initialized, on first mouse move. However, this
// function is intended to be used as a detector for the test harness taking too
// long for some tests, so it's ok to be conservative.
function msSinceMouseOver(timingInfo) {
  return performance.now() - timingInfo.started;
}
async function waitForHoverTime(hoverWaitTimeMs) {
  await new Promise(resolve => step_timeout(resolve,hoverWaitTimeMs));
  await waitForRender();
};

async function createPopoverAndInvokerForHoverTests(test, showdelayMs, hideDelayMs) {
  const unrelated = document.createElement('div');
  unrelated.tabIndex = 0;
  document.body.appendChild(unrelated);
  unrelated.textContent = 'Unrelated';
  unrelated.setAttribute('style','position:fixed; top:0;');
  // Ensure we never hover over or focus on an active interestfor element.
  unrelated.focus();
  await hoverOver(unrelated);
  const popover = document.createElement('div');
  popover.popover = 'auto';
  popover.setAttribute('style','inset:auto; top: 100px;');
  popover.textContent = 'Popover';
  document.body.appendChild(popover);
  let invoker = document.createElement('button');
  invoker.interestForElement = popover;
  invoker.setAttribute('style',`
    interest-delay-start: ${showdelayMs}ms;
    interest-delay-end: ${hideDelayMs}ms;
    position:fixed;
    top:200px;
    width:fit-content;
    height:fit-content;
    `);
  invoker.innerText = 'Invoker';
  document.body.appendChild(invoker);
  const actualShowDelay = Number(getComputedStyle(invoker).interestDelayStart.slice(0,-1))*1000;
  assert_equals(actualShowDelay,showdelayMs,'interest-delay-start is incorrect');
  const actualHideDelay = Number(getComputedStyle(invoker).interestDelayEnd.slice(0,-1))*1000;
  assert_equals(actualHideDelay,hideDelayMs,'interest-delay-end is incorrect');
  test.add_cleanup(() => {
    popover.remove();
    invoker.remove();
    unrelated.remove();
  });
  assert_false(popover.matches(':popover-open'),'The popover should start out closed');
  return {popover, invoker, unrelated};
}
async function sendLoseInterestHotkey() {
  const kEscape = '\uE00C';
  await new test_driver.Actions()
    .keyDown(kEscape)
    .keyUp(kEscape)
    .send();
  await waitForRender();
}
