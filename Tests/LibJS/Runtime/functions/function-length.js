test("basic functionality", () => {
    function foo() {}
    expect(foo).toHaveLength(0);
    expect((foo.length = 5)).toBe(5);
    expect(foo).toHaveLength(0);

    function bar(a, b, c) {}
    expect(bar).toHaveLength(3);
    expect((bar.length = 5)).toBe(5);
    expect(bar).toHaveLength(3);
});

test("functions with special parameter lists", () => {
    function baz(a, b = 1, c) {}
    expect(baz).toHaveLength(1);
    expect((baz.length = 5)).toBe(5);
    expect(baz).toHaveLength(1);

    function qux(a, b, ...c) {}
    expect(qux).toHaveLength(2);
    expect((qux.length = 2)).toBe(2);
    expect(qux).toHaveLength(2);
});
