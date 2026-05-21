import "./async-evaluation-order-direct-1.mjs";
import "./async-evaluation-order-direct-2.mjs";
import "./async-evaluation-order-indirect.mjs";

export const passed = globalThis.asyncEvaluationOrder === "async:direct-1:direct-2:indirect";
