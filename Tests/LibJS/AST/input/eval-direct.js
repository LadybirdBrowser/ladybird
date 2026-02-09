function with_eval() {
    let x = 1;
    eval("");
    return x;
}

function eval_in_nested_scope() {
    let x = 1;
    {
        eval("");
    }
    return x;
}

function nested_function_unaffected() {
    eval("");
    function inner() {
        let y = 2;
        return y;
    }
    return inner();
}
