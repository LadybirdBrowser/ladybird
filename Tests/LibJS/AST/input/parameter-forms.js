function rest_params(a, ...rest) {
    return a + rest.length;
}

function default_params(a, b = 10) {
    return a + b;
}

function destructured_array([x, y]) {
    return x + y;
}

function destructured_object({ a, b }) {
    return a + b;
}

function mixed(first, { x }, ...rest) {
    return first + x + rest.length;
}
