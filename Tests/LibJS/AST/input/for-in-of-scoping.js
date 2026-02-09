// for-in with let: loop variable is block-scoped per iteration.
function for_in_let(obj) {
    for (let key in obj) {
        key;
    }
}

// for-of with let: same block scoping.
function for_of_let(arr) {
    for (let item of arr) {
        item;
    }
}

// for-in with var: variable is function-scoped.
function for_in_var(obj) {
    for (var key in obj) {
        key;
    }
    return key;
}

// for-of with destructuring.
function for_of_destruct(pairs) {
    for (let [a, b] of pairs) {
        a + b;
    }
}

// for-in with eval inside: poisons the loop variable.
function for_in_eval(obj) {
    for (let key in obj) {
        eval("");
        key;
    }
}
