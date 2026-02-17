// Test that array destructuring with rest element produces correct bytecode:
// - Register allocation for rest elements matches C++ double-allocation pattern
// - Correct handling of first vs non-first rest elements

function f() {
    let [a, ...b] = [1, 2, 3];
    return b;
}
f();
