// Installing code in an inner recursive frame must also give the suspended outer frame a discard budget.
// The inner frame returns without using its budget, while the outer frame continues past the code's OSR entry.

function installRecursiveCaller() {
    if (!jit.compile(recursiveLoopsAcrossInstall)) throw new Error("The recursive caller was not compiled");
}
jit.neverCompile(installRecursiveCaller);

function recursiveLoopsAcrossInstall(warm, depth) {
    let sum = 0;
    for (let i = 0; i < 10; ++i) sum += i;
    if (warm) return sum;
    if (depth === 0) {
        installRecursiveCaller();
        return sum;
    }
    recursiveLoopsAcrossInstall(false, depth - 1);
    for (let j = 0; j < 1000000; ++j) {
        if (jit.inCompiledCode()) return -1;
        sum += j & 1;
    }
    return sum;
}
jit.prepare(recursiveLoopsAcrossInstall);
recursiveLoopsAcrossInstall(true, 0);
if (recursiveLoopsAcrossInstall(false, 1) !== -1)
    throw new Error("The outer recursive caller stayed in the interpreter after installation");
const discards = jit.discardCount(recursiveLoopsAcrossInstall);
if (discards !== 1) throw new Error(`The recursive caller code was discarded ${discards} times instead of once`);

// CHECK-FUNCTION: recursiveLoopsAcrossInstall
// CHECK-PASS: graph building
// CHECK: osr entry at pc
