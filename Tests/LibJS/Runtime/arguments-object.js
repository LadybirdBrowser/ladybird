test("basic arguments object", () => {
    function foo() {
        return arguments.length;
    }
    expect(foo()).toBe(0);
    expect(foo(1)).toBe(1);
    expect(foo(1, 2)).toBe(2);
    expect(foo(1, 2, 3)).toBe(3);

    function bar() {
        return arguments[1];
    }
    expect(bar("hello", "friends", ":^)")).toBe("friends");
    expect(bar("hello")).toBe(undefined);
});
