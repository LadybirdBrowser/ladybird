// Spec: https://html.spec.whatwg.org/C/#run-a-module-script
setupTest("Module script queueing a microtask then throwing an exception", [
  "step-2.2-1", "step-2.2-2", // Step 6.
  "microtask-2.2",            // "Clean up after running script" at Step 8.
  "global-error",             // "Clean up after running script" at Step 8,
    // because `evaluationPromise` is synchronously rejected and the rejection
    // is processed in the microtask checkpoint here (See also Step 7).
    // As `evaluationPromise` is rejected after the microtask queued during
    // evaluation, "global-error" occurs after "microtask".
]);
