// Test that computed string keys in object literals like {["x"]: 1}
// are treated the same as non-computed keys like {x: 1}, using
// InitObjectLiteralProperty + CacheObjectShape instead of PutOwnByValue.

function simple_computed() {
    return { ["x"]: 1 };
}
function mixed() {
    return { ["a"]: 1, b: 2 };
}
function dynamic(k) {
    return { [k]: 1 };
}

simple_computed();
mixed();
dynamic("x");
