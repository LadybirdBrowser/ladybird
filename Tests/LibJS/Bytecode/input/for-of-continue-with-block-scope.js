function f(e) {
    const t = [];
    for (const r of e) {
        if (!r) {
            t.push(r);
            continue;
        }
        const o = t.find(i => r.x === i.x);
        o;
    }
    return t;
}
f([]);
