// Catch parameter is scoped to the catch block.
function basic_catch() {
    try {
        throw 1;
    } catch (err) {
        return err;
    }
}

// Catch with destructuring.
function catch_destruct() {
    try {
        throw { msg: "oops", code: 42 };
    } catch ({ msg, code }) {
        return msg + code;
    }
}

// Catch parameter shadows outer variable.
function catch_shadow() {
    let err = "outer";
    try {
        throw "inner";
    } catch (err) {
        err;
    }
    return err;
}

// No catch parameter (catch without binding).
function catch_no_param() {
    try {
        throw 1;
    } catch {
        return 2;
    }
}

// Multiple catch clauses with destructuring patterns.
// Tests that pattern_bound_names don't leak between catch clauses.
function multiple_catch_destruct() {
    try {
        try {
            throw { a: 1 };
        } catch ({ a }) {
            a;
        }
        throw { b: 2 };
    } catch ({ b }) {
        return b;
    }
}

// Catch with eval: poisons the catch block.
function catch_with_eval() {
    try {
        throw 1;
    } catch (err) {
        eval("");
        return err;
    }
}
