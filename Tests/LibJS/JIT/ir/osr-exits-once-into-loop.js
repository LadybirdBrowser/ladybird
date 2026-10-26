// A call that exits once into a long loop that never ran before its code was compiled, like a driver that runs one
// loop after another, does not run the rest of that loop in the interpreter: once it keeps looping where no entry is
// reachable, the code is discarded and compiled again with an entry at that loop.

function runsLoopsInTurn(warm) {
    let sum = 0;
    for (let i = 0; i < 10; ++i) sum += i;
    if (warm) return sum;
    for (let j = 0; j < 100000; ++j) {
        if (jit.inCompiledCode()) return -1;
        sum += j;
    }
    return sum;
}
jit.prepare(runsLoopsInTurn);
runsLoopsInTurn(true);
jit.compile(runsLoopsInTurn);
if (runsLoopsInTurn(false) !== -1) throw new Error("The loop never continued in recompiled code");
if (jit.discardCount(runsLoopsInTurn) !== 1) throw new Error("The code was not discarded once");

// CHECK-FUNCTION: runsLoopsInTurn
// CHECK-PASS: graph building
// CHECK: osr entry at pc
