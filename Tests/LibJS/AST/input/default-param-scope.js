// Default parameter expressions create their own scope.
// The default can reference earlier parameters but not body variables.
function defaults(a, b = a + 1) {
    let c = b;
    return c;
}

// Default parameter with function expression captures the parameter scope.
function default_with_closure(x, f = () => x) {
    let x2 = f();
    return x2;
}

// Default parameter cannot see body `let` declarations (they're in a
// nested scope), but `var` declarations ARE visible to the body.
function complex_defaults(a = 1) {
    var v = a;
    let l = a;
    return v + l;
}

// Destructuring in parameters with defaults.
function destruct_defaults({ x = 10 } = {}) {
    return x;
}
