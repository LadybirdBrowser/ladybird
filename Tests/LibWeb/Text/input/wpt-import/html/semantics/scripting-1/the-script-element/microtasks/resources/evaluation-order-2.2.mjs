globalThis.log.push("step-2.2-1");
queueMicrotask(() => globalThis.log.push("microtask-2.2"));
globalThis.log.push("step-2.2-2");

globalThis.test_load.step_timeout(() => globalThis.testDone(), 0);

throw new Error("error");
