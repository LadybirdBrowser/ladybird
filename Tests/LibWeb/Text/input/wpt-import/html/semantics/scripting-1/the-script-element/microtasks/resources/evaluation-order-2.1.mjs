globalThis.log.push("step-2.1-1");
queueMicrotask(() => globalThis.log.push("microtask-2.1"));
globalThis.log.push("step-2.1-2");

// import is evaluated first.
import "./evaluation-order-2.2.mjs";

globalThis.log.push("step-2.1-3");
