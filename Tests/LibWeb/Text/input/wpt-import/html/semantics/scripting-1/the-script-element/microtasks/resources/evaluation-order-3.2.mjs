globalThis.log.push("step-3.2-1");
queueMicrotask(() => globalThis.log.push("microtask-3.2"));
globalThis.log.push("step-3.2-2");

throw new Error("error");
