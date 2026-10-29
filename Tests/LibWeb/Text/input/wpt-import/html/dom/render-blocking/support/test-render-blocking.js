// Observes the `load` event of an EventTarget, or the finishing of a resource
// given its url. Requires `/preload/resources/preload_helper.js` for the latter
// usage.
class LoadObserver {
  constructor(target) {
    this.finishTime = null;
    this.load = new Promise((resolve, reject) => {
      if (target.addEventListener) {
        target.addEventListener('load', ev => {
          this.finishTime = ev.timeStamp;
          resolve(ev);
        });
        target.addEventListener('error', reject);
      } else if (typeof target === 'string') {
        const observer = new PerformanceObserver(() => {
          if (numberOfResourceTimingEntries(target)) {
            this.finishTime = performance.now();
            resolve();
          }
        });
        observer.observe({type: 'resource', buffered: true});
      } else {
        reject('Unsupported target for LoadObserver');
      }
    });
  }

  get finished() {
    return this.finishTime !== null;
  }
}

// Observes the insertion of a script/parser-blocking element into DOM via
// MutationObserver, so that we can access the element before it's loaded.
function nodeInserted(parentNode, predicate) {
  return new Promise(resolve => {
    function callback(mutationList) {
      for (let mutation of mutationList) {
        for (let node of mutation.addedNodes) {
          if (predicate(node))
            resolve(node);
        }
      }
    }
    new MutationObserver(callback).observe(parentNode, {childList: true});
  });
}

function createAutofocusTarget() {
  const autofocusTarget = document.createElement('textarea');
  autofocusTarget.setAttribute('autofocus', '');
  // We may not have a body element at this point if we are testing a
  // script-blocking stylesheet. Hence, the new element is added to
  // documentElement.
  document.documentElement.appendChild(autofocusTarget);
  return autofocusTarget;
}

function createScrollTarget() {
  const scrollTarget = document.createElement('div');
  scrollTarget.style.overflow = 'scroll';
  scrollTarget.style.height = '100px';
  const scrollContent = document.createElement('div');
  scrollContent.style.height = '200px';
  scrollTarget.appendChild(scrollContent);
  document.documentElement.appendChild(scrollTarget);
  return scrollTarget;
}

function createAnimationTarget() {
  const style = document.createElement('style');
  style.textContent = `
      @keyframes anim {
        from { height: 100px; }
        to { height: 200px; }
      }
  `;
  const animationTarget = document.createElement('div');
  animationTarget.style.backgroundColor = 'green';
  animationTarget.style.height = '50px';
  animationTarget.style.animation = 'anim 100ms';
  document.documentElement.appendChild(style);
  document.documentElement.appendChild(animationTarget);
  return animationTarget;
}

// Error margin for comparing timestamps of paint and load events, in case they
// are reported by different threads.
const epsilon = 50;

function test_render_blocking(optionalElementOrUrl, finalTest, finalTestTitle) {
  // Ideally, we should observe the 'load' event on the specific render-blocking
  // elements. However, this is not possible for script-blocking stylesheets, so
  // we have to observe the 'load' event on 'window' instead.
  if (!(optionalElementOrUrl instanceof HTMLElement) &&
      typeof optionalElementOrUrl !== 'string') {
    finalTestTitle = finalTest;
    finalTest = optionalElementOrUrl;
    optionalElementOrUrl = undefined;
  }
  const loadObserver = new LoadObserver(optionalElementOrUrl || window);

  promise_test(async test => {
    assert_implements(window.PerformancePaintTiming);

    await test.step_wait(() => performance.getEntriesByType('paint').length);

    assert_true(loadObserver.finished);
    for (let entry of performance.getEntriesByType('paint')) {
      assert_greater_than(entry.startTime, loadObserver.finishTime - epsilon,
                          `${entry.name} should occur after loading render-blocking resources`);
    }
  }, 'Rendering is blocked before render-blocking resources are loaded');

  promise_test(test => {
    return loadObserver.load.then(() => finalTest(test));
  }, finalTestTitle);
}
