// Test that async generator yield produces correct bytecode:
// await-before-yield, yield, unwrap-yield-resumption, then completion checking.

async function* f(x) {
    yield x;
}
f(1);
