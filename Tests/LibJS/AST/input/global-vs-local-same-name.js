// A top-level var is global. A parameter with the same name is local.
var x = 1;

function reads_global() {
    return x;
}

function shadows_with_param(x) {
    return x;
}

function shadows_with_var() {
    var x = 2;
    return x;
}

function shadows_with_let() {
    let x = 3;
    return x;
}

// Nested: inner function sees the local from outer, not the global.
function outer(x) {
    function inner() {
        return x;
    }
    return inner();
}
