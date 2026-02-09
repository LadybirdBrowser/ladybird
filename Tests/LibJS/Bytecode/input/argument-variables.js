// Test that simple function parameters are promoted to register-backed
// arguments. The bytecode should use argN registers directly.

function identity(x) {
    return x;
}

function swap(a, b) {
    return b + a;
}

identity(1);
swap(1, 2);
