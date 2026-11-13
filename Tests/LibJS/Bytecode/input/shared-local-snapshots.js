function sink(...values) {
    return values;
}

function beforeWrite(x) {
    return sink(x, x, x, x++);
}

function beforeBranch(x, y, flag) {
    return sink(x, y, x, y, flag ? (x = 2) : (y = 3));
}

beforeWrite(1);
beforeBranch(1, 2, true);
