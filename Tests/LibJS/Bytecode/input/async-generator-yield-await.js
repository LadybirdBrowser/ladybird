// Test that yield await x in an async generator produces correct bytecode.

async function* f(x) {
    yield await x;
}
f(1);
