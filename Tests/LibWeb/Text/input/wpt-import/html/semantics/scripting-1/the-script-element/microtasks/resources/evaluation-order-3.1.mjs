globalThis.log.push("step-3.1-1");
queueMicrotask(() => globalThis.log.push("microtask-3.1"));
globalThis.log.push("step-3.1-2");

import("./evaluation-order-3.2.mjs").catch(
    exception => {
      globalThis.log.push("import-catch", exception.message);
      globalThis.testDone();
    });

globalThis.log.push("step-3.1-3");
