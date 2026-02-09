// Test that variables captured by nested functions are NOT promoted to locals.
// The bytecode should use GetBinding/SetBinding for captured variables.

function outer() {
    let captured = 1;
    function inner() {
        return captured;
    }
    return inner();
}

outer();
