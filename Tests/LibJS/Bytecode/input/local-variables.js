// Test that simple local variables (let/const) are promoted to register-backed
// locals. The bytecode should use GetLocal/SetLocal, not GetBinding/SetBinding.

function simple_let() {
    let x = 1;
    return x;
}

function simple_const() {
    const y = 2;
    return y;
}

function multiple_locals() {
    let a = 1;
    let b = 2;
    return a + b;
}

simple_let();
simple_const();
multiple_locals();
