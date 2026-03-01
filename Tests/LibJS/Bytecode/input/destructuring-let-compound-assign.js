function f(t, e) {
    let { patchFlag: a } = t;
    a |= 16 & e.patchFlag;
}
f({}, {});
