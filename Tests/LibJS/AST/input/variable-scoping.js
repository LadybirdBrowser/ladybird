function var_hoisting() {
    x = 1;
    var x;
    return x;
}

function let_in_block() {
    let x = 1;
    {
        let x = 2;
        x;
    }
    return x;
}

function const_binding() {
    const x = 1;
    const y = 2;
    return x + y;
}

function for_let_scoping() {
    let sum = 0;
    for (let i = 0; i < 3; i++) {
        sum = sum + i;
    }
    return sum;
}
