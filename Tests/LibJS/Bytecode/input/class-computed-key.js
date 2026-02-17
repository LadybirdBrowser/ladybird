// Test that class with computed property key uses correct register ordering.

function f() {
    return class {
        *[Symbol.iterator]() {
            yield 1;
        }
    };
}
f();
