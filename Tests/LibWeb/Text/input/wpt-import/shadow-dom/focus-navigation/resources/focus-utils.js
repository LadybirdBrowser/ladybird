'use strict';

// --- Generic focus testing primitives ---

function waitForRender() {
  return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
}

// https://w3c.github.io/webdriver/#keyboard-actions
const kArrowLeft = '\uE012';
const kArrowUp = '\uE013';
const kArrowRight = '\uE014';
const kArrowDown = '\uE015';
const kHome = '\uE011';
const kEnd = '\uE010';
const kTab = '\uE004';
const kShift = '\uE008';

// Focus an element and wait for the focus state to propagate via double-rAF.
// Use this instead of bare element.focus() to prevent flaky failures on
// slower CI environments where focus may not be fully established before
// subsequent key dispatches.
async function focusAndWait(element) {
  element.focus();
  await waitForRender();
}

// Focus target and send a key press from it. Waits one animation frame after
// focusing to ensure focus is established before dispatching the key.
async function focusAndKeyPress(target, key) {
  target.focus();
  await new Promise(resolve => requestAnimationFrame(resolve));
  return test_driver.send_keys(target, key);
}

// Send a single key press using the Actions API. Does not change focus before
// dispatching, unlike test_driver.send_keys().
function sendKey(key) {
  return new test_driver.Actions().keyDown(key).keyUp(key).send();
}

// Send Tab using the Actions API without targeting a specific element.
// Unlike navigateFocusForward, this does NOT call focus() on any element
// first, so it will not trigger blur events that could interfere with
// focusgroup exit behaviour on key-conflict elements.
async function sendTabForward() {
  await waitForRender();
  await sendKey(kTab);
  await waitForRender();
}

async function navigateFocusForward() {
  await waitForRender();
  // Use test_driver.send_keys for reliable synchronization rather than the
  // lower-level Actions API which can drop key events on slower CI
  // environments.  We target document.body because body.focus() is a no-op,
  // which preserves the current focus state. Targeting activeElement would
  // break shadow hosts with delegatesFocus (re-delegating on focus()).
  await test_driver.send_keys(document.body, kTab);
  await waitForRender();
}

// Shift+Tab via the Actions API.  Does not call focus() on any element, so
// it will not trigger blur events that could interfere with focusgroup exit
// behaviour on key-conflict elements.
async function navigateFocusBackward() {
  await waitForRender();
  await new test_driver.Actions()
    .keyDown(kShift)
    .keyDown(kTab)
    .keyUp(kTab)
    .keyUp(kShift)
    .send();
  await waitForRender();
}

// If shadow root is open, can find element using element path
// If shadow root is open, can find the shadowRoot from the element

function innermostActiveElement(element) {
  element = element || document.activeElement;
  if (isIFrameElement(element)) {
    if (element.contentDocument.activeElement)
      return innermostActiveElement(element.contentDocument.activeElement);
    return element;
  }
  if (isShadowHost(element)) {
    let shadowRoot = element.shadowRoot;
    if (shadowRoot) {
      if (shadowRoot.activeElement)
        return innermostActiveElement(shadowRoot.activeElement);
    }
  }
  return element;
}

function isInnermostActiveElement(path) {
  const element = getNodeInComposedTree(path);
  if (!element)
    return false;
  return element === innermostActiveElement();
}

async function shouldNavigateFocus(fromElement, direction) {
  if (!fromElement)
    return false;

  fromElement.focus();
  if (fromElement !== innermostActiveElement())
    return false;

  if (direction == 'forward')
    await navigateFocusForward();
  else
    await navigateFocusBackward();

  return true;
}

async function assert_focus_navigation_element(fromPath, toPath, direction) {
  const fromElement = getNodeInComposedTree(fromPath);
  const result = await shouldNavigateFocus(fromElement, direction);
  assert_true(result, 'Failed to focus ' + fromPath);

  const message =
    `Focus should move ${direction} from ${fromPath} to ${toPath}`;
  const toElement = getNodeInComposedTree(toPath);
  assert_equals(innermostActiveElement(), toElement, message);
}

async function assert_focus_navigation_elements(elements, direction) {
  assert_true(
    elements.length >= 2,
    'length of elements should be greater than or equal to 2.');
  for (var i = 0; i + 1 < elements.length; ++i)
    await assert_focus_navigation_element(elements[i], elements[i + 1], direction);

}

async function assert_focus_navigation_forward(elements) {
  return assert_focus_navigation_elements(elements, 'forward');
}

async function assert_focus_navigation_backward(elements) {
  return assert_focus_navigation_elements(elements, 'backward');
}

async function assert_focus_navigation_bidirectional(elements) {
  await assert_focus_navigation_forward(elements);
  elements.reverse();
  await assert_focus_navigation_backward(elements);
}


// If shadow root is closed, need to pass shadowRoot and element to find
// innermost active element

function isShadowHostOfRoot(shadowRoot, node) {
  return shadowRoot && shadowRoot.host.isEqualNode(node);
}

function innermostActiveElementWithShadowRoot(shadowRoot, element) {
  element = element || document.activeElement;
  if (isIFrameElement(element)) {
    if (element.contentDocument.activeElement)
      return innermostActiveElementWithShadowRoot(shadowRoot, element.contentDocument.activeElement);
    return element;
  }
  if (isShadowHostOfRoot(shadowRoot, element)) {
    if (shadowRoot.activeElement)
      return innermostActiveElementWithShadowRoot(shadowRoot, shadowRoot.activeElement);
  }
  return element;
}

async function shouldNavigateFocusWithShadowRoot(from, direction) {
  const [fromElement, shadowRoot] = from;
  if (!fromElement)
    return false;

  fromElement.focus();
  if (fromElement !== innermostActiveElementWithShadowRoot(shadowRoot))
    return false;

  if (direction == 'forward')
    await navigateFocusForward();
  else
    await navigateFocusBackward();

  return true;
}

async function assert_focus_navigation_element_with_shadow_root(from, to, direction) {
  const result = await shouldNavigateFocusWithShadowRoot(from, direction);
  const [fromElement] = from;
  const [toElement, toShadowRoot] = to;
  assert_true(result, 'Failed to focus ' + fromElement.id);
  const message =
    `Focus should move ${direction} from ${fromElement.id} to ${toElement.id}`;
  assert_equals(innermostActiveElementWithShadowRoot(toShadowRoot), toElement, message);
}

async function assert_focus_navigation_elements_with_shadow_root(elements, direction) {
  assert_true(
    elements.length >= 2,
    'length of elements should be greater than or equal to 2.');
  for (var i = 0; i + 1 < elements.length; ++i)
    await assert_focus_navigation_element_with_shadow_root(elements[i], elements[i + 1], direction);
}

async function assert_focus_navigation_forward_with_shadow_root(elements) {
  return assert_focus_navigation_elements_with_shadow_root(elements, 'forward');
}

async function assert_focus_navigation_backward_with_shadow_root(elements) {
  return assert_focus_navigation_elements_with_shadow_root(elements, 'backward');
}

async function assert_focus_navigation_bidirectional_with_shadow_root(elements) {
  await assert_focus_navigation_forward_with_shadow_root(elements);
  elements.reverse();
  await assert_focus_navigation_backward_with_shadow_root(elements);
}

// This Promise will run each test case that is:
// 1. Wrapped in an element with class name "test-case".
// 2. Has data-expect attribute be an ordered list of elements to focus.
// 3. Has data-description attribute be a string explaining the test.
// e.g <div class="test-case" data-expect="b,a,c"
//          data-description="Focus navigation">
async function runFocusTestCases() {
  const testCases = Array.from(document.querySelectorAll('.test-case'));
  for (let testCase of testCases) {
    promise_test(async () => {
      const expected = testCase.dataset.expect.split(',');
      await assert_focus_navigation_bidirectional(expected);
    }, testCase.dataset.description);
  }
}
