test("basic functionality", () => {
    function foo(a, b) {
        return a + b;
    }

    expect(foo()).toBeNaN();
    expect(foo(1)).toBeNaN();
    expect(foo(2, 3)).toBe(5);
    expect(foo(2, 3, 4)).toBe(5);
});
