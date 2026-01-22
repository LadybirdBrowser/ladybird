test("basic functionality", () => {
    const x = 1;

    expect(x === 1 ? true : false).toBeTrue();
    expect(x ? x : 0).toBe(x);
    expect(1 < 2 ? true : false).toBeTrue();
    expect(0 ? 1 : 1 ? 10 : 20).toBe(10);
    expect(0 ? (1 ? 1 : 10) : 20).toBe(20);
});

test("object values", () => {
    const o = {};
    o.f = true;
    expect(o.f ? true : false).toBeTrue();
    expect(1 ? o.f : null).toBeTrue();
});

test("issue #4409, '?.' followed by decimal digit", () => {
    expect("false?.1:.2").toEvalTo(0.2);
});
