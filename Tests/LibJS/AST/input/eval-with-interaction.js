// eval inside with: both poisons apply.
function eval_in_with(obj) {
    let a = 1;
    with (obj) {
        eval("");
        a;
    }
}

// with inside eval-poisoned function.
function with_after_eval(obj) {
    let b = 1;
    eval("");
    with (obj) {
        b;
    }
}

// eval in one branch, with in another -- both poison the function.
function branched_poison(flag, obj) {
    let c = 1;
    if (flag) {
        eval("");
    } else {
        with (obj) {
            c;
        }
    }
    return c;
}
