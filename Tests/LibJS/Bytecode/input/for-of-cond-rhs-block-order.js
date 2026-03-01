function f(o, i) {
    for (let [r, n] of (o ? o.x : i, Object.entries({}))) {
        r;
    }
}
f(null, null);
