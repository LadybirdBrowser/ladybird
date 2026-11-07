const a = 1;
const two = 2;
const three = 3;

export { a, two as "a\u0000", three as "a\u0000\u0000" };
export { a as "ab", two as "ab\u0000", three as "ab\u0000\u0000" };
export { a as "", two as "\u0000", three as "\u0000\u0000" };
export { three as "only\u0000\u0000" };

// Intern the NUL-containing name before the matching identifier. A lazy reparse
// of readB must still find the identifier's original ID.
export { two as "b\u0000\u0000", b };
const b = 4;
export function readB() {
    return b;
}

export const passed = true;
