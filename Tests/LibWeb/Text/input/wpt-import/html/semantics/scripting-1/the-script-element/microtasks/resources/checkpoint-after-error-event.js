// The `then` handlers for `Promise.resolve()` are evaluated in the first
// microtasks checkpoint after `Promise.resolve()`.

self.setup({allow_uncaught_exception: true});

// ----------------------------------------------------------------
// Check when microtasks checkpoint is performed
// - Around `self`'s error event fired via
//   https://html.spec.whatwg.org/C/#report-the-error during
//   - https://html.spec.whatwg.org/C/#run-a-classic-script or
//   - https://html.spec.whatwg.org/C/#run-a-module-script.

// The expectation is: the `then` handlers are evaluated after all error event
// handlers are evaluated, not after each error event handler.

// Just after each event handler is invoked,
// https://webidl.spec.whatwg.org/#call-a-user-objects-operation
// calls #clean-up-after-running-script, but this doesn't execute new
// microtasks immediately, because:
// - Before https://github.com/whatwg/html/pull/4352:
//   #report-the-error is called before #clean-up-after-running-script by
//   #run-a-classic-script/#run-a-module-script, so microtask checkpoint
//   is not performed because JavaScript execution context stack is not empty.
// - After https://github.com/whatwg/html/pull/4352:
//   #report-the-error is called during #perform-a-microtask-checkpoint (because
//   it is called on rejection of promises), so #perform-a-microtask-checkpoint
//   is executed but early exited.
self.log = [];

self.addEventListener('error', () => {
  log.push('handler 1');
  Promise.resolve().then(() => log.push('handler 1 promise'));
});
self.addEventListener('error', () => {
  log.push('handler 2');
  Promise.resolve().then(() => log.push('handler 2 promise'));
});

// Microtasks should be executed before
// #run-a-classic-script/#run-a-module-script is completed, and thus before
// script evaluation scheduled by setTimeout().
async_test(t => {
  t.step_timeout(() => {
      assert_array_equals(log, [
          'handler 1',
          'handler 2',
          'handler 1 promise',
          'handler 2 promise'
        ]);
      t.done();
    },
    0);
}, "Promise resolved during #report-the-error");

// ----------------------------------------------------------------
// Check when microtasks checkpoint is performed
// around event events other than the `self` error event cases above.
// In this case, microtasks are executed just after each event handler is
// invoked via #clean-up-after-running-script called from
// https://webidl.spec.whatwg.org/#call-a-user-objects-operation,
// because the event handlers are executed outside the
// #prepare-to-run-script/#clean-up-after-running-script scopes in
// #run-a-classic-script/#run-a-module-script.
self.log2 = [];
self.t2 = async_test(
    "Promise resolved during event handlers other than error");

self.addEventListener('message', () => {
  log2.push('handler 1');
  Promise.resolve().then(() => log2.push('handler 1 promise'));
});
self.addEventListener('message', () => {
  log2.push('handler 2');
  Promise.resolve().then(t2.step_func_done(() => {
      log2.push('handler 2 promise');
      assert_array_equals(log2, [
          'handler 1',
          'handler 1 promise',
          'handler 2',
          'handler 2 promise'
        ]);
    }));
});

// ----------------------------------------------------------------

done();

throw new Error('script 1');
