// `var` is hoisted to the function scope, `let` stays block-scoped.
function hoist_test() {
    {
        var hoisted = 1;
        let blocked = 2;
    }
    return hoisted;
}

// `var` inside a for loop is function-scoped.
function for_var() {
    for (var i = 0; i < 10; i++) {
        var inner = i;
    }
    return i + inner;
}

// `let` inside a for loop is block-scoped.
function for_let() {
    for (let j = 0; j < 10; j++) {}
}

// `var` in catch block is function-scoped.
function catch_var() {
    try {
        throw 1;
    } catch (e) {
        var caught = e;
    }
    return caught;
}
