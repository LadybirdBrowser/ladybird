function f(x) {
    let a = (++x) ** 2;
    let b = (--x) ** 2;
    let c = (++x) ** 2;
    return a + b + c;
}
