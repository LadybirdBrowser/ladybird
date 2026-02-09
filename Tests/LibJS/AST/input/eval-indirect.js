// Indirect eval (not a direct call to `eval`) does NOT poison scope.
function indirect_eval() {
    let x = 1;
    (0, eval)("x");
    return x;
}

// Assigning eval to a variable and calling it is also indirect.
function eval_alias() {
    let x = 1;
    var e = eval;
    e("x");
    return x;
}

// eval as a method call is indirect.
function eval_method() {
    let x = 1;
    var obj = { eval };
    obj.eval("x");
    return x;
}

// Calling something NAMED eval that isn't the global eval.
function local_eval_name() {
    let x = 1;
    let eval = s => s;
    eval("x");
    return x;
}
