// Direct eval in a function makes all bindings non-local
// because eval could do `var x = ...` and shadow them.
function outer() {
    let a = 1;
    let b = 2;
    eval("");
    return a + b;
}

// Eval in a nested block still poisons the whole function.
function block_eval() {
    let x = 1;
    {
        eval("");
    }
    return x;
}

// Eval in an inner function does NOT poison the outer function.
function outer_safe() {
    let clean = 1;
    function inner() {
        eval("");
    }
    return clean;
}
