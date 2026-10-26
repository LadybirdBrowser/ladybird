// An exit from outer -> middle -> leaf must give the materialized middle frame its own discard budget.
// Only middle has a long loop after the exit; neither the compiled root nor the innermost frame can use its budget.

function leafForLoop(value) {
    return value + 1;
}
function middleWithLoop(warm, value) {
    let sum = 0;
    for (let i = 0; i < 10; ++i) sum += i;
    leafForLoop(value);
    if (warm) return sum;
    for (let j = 0; j < 1000000; ++j) {
        if (jit.inCompiledCode()) return -1;
        sum += j & 1;
    }
    return sum;
}
function outerWithLoop(warm, value) {
    return middleWithLoop(warm, value);
}
jit.prepare(leafForLoop);
jit.prepare(middleWithLoop);
jit.prepare(outerWithLoop);
outerWithLoop(true, 1);
if (!jit.compile(middleWithLoop) || !jit.compile(outerWithLoop)) throw new Error("The call chain was not compiled");
if (outerWithLoop(false, "different type") !== -1)
    throw new Error("The intermediate inlined frame stayed in the interpreter");
if (jit.discardCount(middleWithLoop) !== 1) throw new Error("The intermediate code was not discarded once");

// CHECK-FUNCTION: outerWithLoop
// CHECK-PASS: graph building
// CHECK: Int32Add
// CHECK: fs[[MIDDLE:[0-9]+]]: e1 pc {{[0-9]+}} after
// CHECK: e2 pc {{.*}} in fs[[MIDDLE]]
