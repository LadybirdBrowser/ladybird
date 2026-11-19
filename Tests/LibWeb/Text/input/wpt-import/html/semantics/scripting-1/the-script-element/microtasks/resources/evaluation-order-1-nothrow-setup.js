// Spec: https://html.spec.whatwg.org/C/#run-a-module-script
setupTest("Module script queueing a microtask", [
  "body",         // Step 6.
  "microtask",    // "Clean up after running script" at Step 8.
]);
