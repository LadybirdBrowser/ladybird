queueMicrotask(() => globalThis.log.push("microtask"));
globalThis.log.push("body");
