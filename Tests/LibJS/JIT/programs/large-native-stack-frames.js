// JIT code passes the arguments of some calls on the native stack, and keeps the operands of some slow paths in its
// native stack frame. Neither may skip over the guard pages that Windows keeps below the committed part of a thread's
// stack, which fault when skipped. The second use is larger than the first, so that it also goes beyond what was ever
// used by more than the guard pages.
function operands(count) {
    return Array.from({ length: count }, () => "value").join(", ");
}

// NB: A call of a native function goes through the call stub, which takes its arguments on the native stack.
const callWithManyArguments = Function("callee", "value", `return callee(${operands(24000)});`);

// NB: An array literal has its elements in the operand record of its slow path, in the frame. The frames also have to
//     get past each other, and are larger than what JIT code leaves of the native stack when it stops calling
//     deeper, so that a frame that recursing runs into the end of the stack with must not be touched at all.
const createLargeArray = Function(
    "value",
    "depth",
    `
    const below = depth > 0 ? createLargeArray(value, depth - 1) : 0;
    const array = [${operands(64000)}];
    return below + array.length;
`
);

for (let warmup = 0; warmup < 500; warmup++) {
    callWithManyArguments(Math.max, warmup);
    createLargeArray(warmup, 0);
}

print(callWithManyArguments(Math.max, 1));
for (const depth of [1, 2, 4, 8]) print(createLargeArray(1, depth));

try {
    createLargeArray(1, Infinity);
} catch (error) {
    print(error instanceof InternalError);
}
