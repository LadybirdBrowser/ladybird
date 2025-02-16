// Here's how waitForNotification works:
//
// - myTestFunction0()
//   - waitForNotification(myTestFunction1)
//     - requestAnimationFrame()
//   - Modify DOM in a way that should trigger an IntersectionObserver callback.
// - BeginFrame
//   - requestAnimationFrame handler runs
//     - Second requestAnimationFrame()
//   - Style, layout, paint
//   - IntersectionObserver generates new notifications
//     - Posts a task to deliver notifications
// - Task to deliver IntersectionObserver notifications runs
//   - IntersectionObserver callbacks run
// - Second requestAnimationFrameHandler runs
//     - step_timeout()
// - step_timeout handler runs
//   - myTestFunction1()
//     - [optional] waitForNotification(myTestFunction2)
//       - requestAnimationFrame()
//     - Verify newly-arrived IntersectionObserver notifications
//     - [optional] Modify DOM to trigger new notifications
//
// Ideally, it should be sufficient to use requestAnimationFrame followed
// by two step_timeouts, with the first step_timeout firing in between the
// requestAnimationFrame handler and the task to deliver notifications.
// However, the precise timing of requestAnimationFrame, the generation of
// a new display frame (when IntersectionObserver notifications are
// generated), and the delivery of these events varies between engines, making
// this tricky to test in a non-flaky way.
//
// In particular, in WebKit, requestAnimationFrame and the generation of
// a display frame are two separate tasks, so a step_timeout called within
// requestAnimationFrame can fire before a display frame is generated.
//
// In Gecko, on the other hand, requestAnimationFrame and the generation of
// a display frame are a single task, and IntersectionObserver notifications
// are generated during this task. However, the task posted to deliver these
// notifications can fire after the following requestAnimationFrame.
//
// This means that in general, by the time the second requestAnimationFrame
// handler runs, we know that IntersectionObservations have been generated,
// and that a task to deliver these notifications has been posted (though
// possibly not yet delivered). Then, by the time the step_timeout() handler
// runs, these notifications have been delivered.
//
// Since waitForNotification uses a double-rAF, it is now possible that
// IntersectionObservers may have generated more notifications than what is
// under test, but have not yet scheduled the new batch of notifications for
// delivery. As a result, observer.takeRecords should NOT be used in tests:
//
// - myTestFunction0()
//   - waitForNotification(myTestFunction1)
//     - requestAnimationFrame()
//   - Modify DOM in a way that should trigger an IntersectionObserver callback.
// - BeginFrame
//   - requestAnimationFrame handler runs
//     - Second requestAnimationFrame()
//   - Style, layout, paint
//   - IntersectionObserver generates a batch of notifications
//     - Posts a task to deliver notifications
// - Task to deliver IntersectionObserver notifications runs
//   - IntersectionObserver callbacks run
// - BeginFrame
//   - Second requestAnimationFrameHandler runs
//     - step_timeout()
//   - IntersectionObserver generates another batch of notifications
//     - Post task to deliver notifications
// - step_timeout handler runs
//   - myTestFunction1()
//     - At this point, observer.takeRecords will get the second batch of
//       notifications.
function waitForNotification(t, f) {
  return new Promise(resolve => {
    requestAnimationFrame(function() {
      requestAnimationFrame(function() {
        let callback = function() {
          resolve();
          if (f) {
            f();
          }
        };
        if (t) {
          t.step_timeout(callback);
        } else {
          setTimeout(callback);
        }
      });
    });
  });
}

// If you need to wait until the IntersectionObserver algorithm has a chance
// to run, but don't need to wait for delivery of the notifications...
function waitForFrame(t, f) {
  return new Promise(resolve => {
    requestAnimationFrame(function() {
      t.step_timeout(function() {
        resolve();
        if (f) {
          f();
        }
      });
    });
  });
}

// The timing of when runTestCycle is called is important.  It should be
// called:
//
//   - Before or during the window load event, or
//   - Inside of a prior runTestCycle callback, *before* any assert_* methods
//     are called.
//
// Following these rules will ensure that the test suite will not abort before
// all test steps have run.
//
// If the 'delay' parameter to the IntersectionObserver constructor is used,
// tests will need to add the same delay to their runTestCycle invocations, to
// wait for notifications to be generated and delivered.
function runTestCycle(f, description, delay) {
  async_test(function(t) {
    if (delay) {
      step_timeout(() => {
        waitForNotification(t, t.step_func_done(f));
      }, delay);
    } else {
      waitForNotification(t, t.step_func_done(f));
    }
  }, description);
}

// Root bounds for a root with an overflow clip as defined by:
//   http://wicg.github.io/IntersectionObserver/#intersectionobserver-root-intersection-rectangle
function contentBounds(root) {
  var left = root.offsetLeft + root.clientLeft;
  var right = left + root.clientWidth;
  var top = root.offsetTop + root.clientTop;
  var bottom = top + root.clientHeight;
  return [left, right, top, bottom];
}

// Root bounds for a root without an overflow clip as defined by:
//   http://wicg.github.io/IntersectionObserver/#intersectionobserver-root-intersection-rectangle
function borderBoxBounds(root) {
  var left = root.offsetLeft;
  var right = left + root.offsetWidth;
  var top = root.offsetTop;
  var bottom = top + root.offsetHeight;
  return [left, right, top, bottom];
}

function clientBounds(element) {
  var rect = element.getBoundingClientRect();
  return [rect.left, rect.right, rect.top, rect.bottom];
}

function rectArea(rect) {
  return (rect.left - rect.right) * (rect.bottom - rect.top);
}

function checkRect(actual, expected, description, epsilon = 0) {
  if (!expected.length)
    return;
  assert_approx_equals(actual.left, expected[0], epsilon, description + '.left');
  assert_approx_equals(actual.right, expected[1], epsilon, description + '.right');
  assert_approx_equals(actual.top, expected[2], epsilon, description + '.top');
  assert_approx_equals(actual.bottom, expected[3], epsilon, description + '.bottom');
}

function checkLastEntry(entries, i, expected, epsilon = 0) {
  assert_equals(entries.length, i + 1, 'entries.length');
  if (expected) {
    checkRect(
        entries[i].boundingClientRect, expected.slice(0, 4),
        'entries[' + i + '].boundingClientRect', epsilon);
    checkRect(
        entries[i].intersectionRect, expected.slice(4, 8),
        'entries[' + i + '].intersectionRect', epsilon);
    checkRect(
        entries[i].rootBounds, expected.slice(8, 12),
        'entries[' + i + '].rootBounds', epsilon);
    if (expected.length > 12) {
      assert_equals(
          entries[i].isIntersecting, expected[12],
          'entries[' + i + '].isIntersecting');
    }
  }
}

function checkJsonEntry(actual, expected) {
  checkRect(
      actual.boundingClientRect, expected.boundingClientRect,
      'entry.boundingClientRect');
  checkRect(
      actual.intersectionRect, expected.intersectionRect,
      'entry.intersectionRect');
  if (actual.rootBounds == 'null')
    assert_equals(expected.rootBounds, 'null', 'rootBounds is null');
  else
    checkRect(actual.rootBounds, expected.rootBounds, 'entry.rootBounds');
  assert_equals(actual.isIntersecting, expected.isIntersecting);
  assert_equals(actual.target, expected.target);
}

function checkJsonEntries(actual, expected, description) {
  test(function() {
    assert_equals(actual.length, expected.length);
    for (var i = 0; i < actual.length; i++)
      checkJsonEntry(actual[i], expected[i]);
  }, description);
}

function checkIsIntersecting(entries, i, expected) {
  assert_equals(entries[i].isIntersecting, expected,
    'entries[' + i + '].target.isIntersecting equals ' + expected);
}
