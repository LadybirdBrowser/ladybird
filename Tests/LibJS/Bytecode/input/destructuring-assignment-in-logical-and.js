function f(t, e) {
    let a, b;
    t && ([a, b] = t(e));
}
f(null, 0);
