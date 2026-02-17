// Test that yield* in a sync generator produces correct bytecode.

function* f(x) {
    yield* x;
}
f([1, 2]);
