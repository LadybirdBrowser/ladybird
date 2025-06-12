test("basic functionality", () => {
    function foo(a) {
        return a;
    }

    var x = undefined;
    expect(x).toBeUndefined();
    expect(foo(x)).toBeUndefined();

    var o = {};
    o.x = x;
    expect(o.x).toBeUndefined();
    expect(o.x).toBe(x);
});
