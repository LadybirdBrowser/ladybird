function uses_arrow() {
    let x = 1;
    const f = y => x + y;
    return f(2);
}

function arrow_with_this() {
    const f = () => this;
    return f();
}

const top_level_arrow = (a, b) => a + b;
