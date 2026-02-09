// `with` makes identifiers ambiguous -- they might come from the
// with-object or from the enclosing scope.
function basic_with(obj) {
    let x = 1;
    with (obj) {
        x;
    }
}

// Variables declared INSIDE a with block are still local to the function.
function var_in_with(obj) {
    with (obj) {
        var y = 2;
    }
    return y;
}

// Nested function inside with inherits the ambiguity.
function nested_in_with(obj) {
    let z = 10;
    with (obj) {
        (function () {
            return z;
        });
    }
}

// with + eval: double poison.
function with_and_eval(obj) {
    let a = 1;
    with (obj) {
        eval("");
        a;
    }
}
