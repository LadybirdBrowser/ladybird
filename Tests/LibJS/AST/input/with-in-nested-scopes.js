// `with` in outer function poisons inner function's identifier resolution.
function outer(obj) {
    let x = 1;
    with (obj) {
        function inner() {
            return x;
        }
        inner();
    }
}

// `with` in outer does NOT poison a sibling function defined outside with.
function sibling_test(obj) {
    function clean() {
        let y = 1;
        return y;
    }
    with (obj) {
        clean();
    }
}

// Nested with statements.
function double_with(a, b) {
    with (a) {
        with (b) {
            x;
        }
    }
}

// with + arrow function.
function with_arrow(obj) {
    let z = 1;
    with (obj) {
        let f = () => z;
    }
}
