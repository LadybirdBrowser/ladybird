log.push("step-4.2-1");
queueMicrotask(() => log.push("microtask-4.2"));
log.push("step-4.2-2");

throw new Error("error");
