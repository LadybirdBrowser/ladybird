// Test that eval in scope chain prevents local variable promotion.
// Variables in a function with eval cannot be locals because eval
// may inject new bindings at runtime.

function with_eval() {
    let x = 1;
    eval("");
    return x;
}

// But variables in a nested function (without eval itself) should
// still be promoted to locals, even if the outer function has eval.
function outer_eval() {
    eval("");
    function inner() {
        let y = 2;
        return y;
    }
    return inner();
}

with_eval();
outer_eval();
