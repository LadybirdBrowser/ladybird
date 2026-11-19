setupTest("Module script queueing a microtask then throwing an exception", [
  "step-3.1-1", "step-3.1-2", "step-3.1-3",
  "microtask-3.1",
  "step-3.2-1", "step-3.2-2",
  "microtask-3.2",
  "import-catch", "error",
]);
