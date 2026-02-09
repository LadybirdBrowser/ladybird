// Destructured parameter captured by a closure loses its local index.
function captured_by_closure({ x }) {
    return function () {
        return x;
    };
}

// Direct eval in the function defeats local indices.
function eval_defeats({ a, b }) {
    eval("");
    return a + b;
}

// `with` statement defeats local indices for identifiers used inside.
function with_defeats({ x }, obj) {
    with (obj) {
        return x;
    }
}

// Accessing `arguments` object prevents parameter locals.
function arguments_access({ x }) {
    return arguments[0];
}

// Plain + destructured: only plain should keep [argument:N] when
// no defeating conditions apply within the same function.
function mixed_no_defeat(first, [a, b]) {
    return first + a + b;
}
