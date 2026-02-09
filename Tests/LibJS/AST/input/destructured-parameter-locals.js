function array_destructured([x, y]) {
    return x + y;
}

function object_destructured({ a, b }) {
    return a + b;
}

function mixed(first, { x }, ...rest) {
    return first + x + rest.length;
}

function nested({ a: { b } }) {
    return b;
}

function with_defaults({ x = 10 } = {}) {
    return x;
}

function aliased({ a: renamed }) {
    return renamed;
}
