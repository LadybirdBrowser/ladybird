// Code that keeps exiting into a loop without an OSR entry is discarded after fewer exits than elsewhere, and
// compiled again with an entry at that loop.

function exitsIntoLoop(cold) {
    let sum = 0;
    for (let i = 0; i < 10; ++i) sum += i;
    if (cold) {
        for (let j = 0; j < 5000; ++j) {
            if (jit.inCompiledCode()) return -1;
            sum += j;
        }
    }
    return sum;
}
jit.prepare(exitsIntoLoop);
exitsIntoLoop(false);
jit.compile(exitsIntoLoop);
let calls = 0;
while (exitsIntoLoop(true) !== -1) {
    if (++calls === 7) throw new Error("The loop never continued in recompiled code");
}
if (jit.discardCount(exitsIntoLoop) !== 1) throw new Error("The code was not discarded once");

// CHECK-FUNCTION: exitsIntoLoop
// CHECK-PASS: graph building
// CHECK: osr entry at pc
