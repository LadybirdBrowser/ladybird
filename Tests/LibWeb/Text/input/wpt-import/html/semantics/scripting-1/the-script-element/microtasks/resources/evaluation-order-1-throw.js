queueMicrotask(() => globalThis.log.push("microtask"));
globalThis.log.push("body");

throw new Error("error");
