// Test that yield inside a finally block saves/restores the exception register.

function* f() {
    try {
        yield 1;
        yield 2;
    } finally {
        yield 3;
    }
}
f();
