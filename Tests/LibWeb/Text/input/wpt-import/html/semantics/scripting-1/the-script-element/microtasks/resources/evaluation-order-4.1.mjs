log.push("step-4.1-1");
queueMicrotask(() => log.push("microtask-4.1"));
log.push("step-4.1-2");

await import("./evaluation-order-4.2.mjs");

// Not happening as we throw in the above module.
log.push("step-4.1-3");
