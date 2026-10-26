// Installing code from a callee must give the suspended interpreted caller a chance to discard it when its next
// loop cannot reach an OSR entry. The caller has never entered the code, so no JIT exit can give it that budget.

function installCaller() {
    if (!jit.compile(runsLoopsAcrossInstall)) throw new Error("The caller was not compiled");
}
jit.neverCompile(installCaller);

function runsLoopsAcrossInstall(warm) {
    let sum = 0;
    for (let i = 0; i < 10; ++i) sum += i;
    if (warm) return sum;
    installCaller();
    for (let j = 0; j < 1000000; ++j) {
        if (jit.inCompiledCode()) return -1;
        sum += j & 1;
    }
    return sum;
}
jit.prepare(runsLoopsAcrossInstall);
runsLoopsAcrossInstall(true);
if (runsLoopsAcrossInstall(false) !== -1) throw new Error("The caller stayed in the interpreter after installation");
const discards = jit.discardCount(runsLoopsAcrossInstall);
if (discards !== 1) throw new Error(`The caller code was discarded ${discards} times instead of once`);

// CHECK-FUNCTION: runsLoopsAcrossInstall
// CHECK-PASS: graph building
// CHECK: osr entry at pc
