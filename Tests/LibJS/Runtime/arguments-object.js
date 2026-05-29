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

test("mapped arguments object accessed from arrow function", () => {
    function outer(value) {
        return (() => arguments[0])();
    }
    expect(outer(42)).toBe(42);
    expect(outer("hello")).toBe("hello");

    function multipleParams(a, b, c) {
        return (() => [arguments[0], arguments[1], arguments[2]])();
    }
    expect(multipleParams(1, 2, 3)).toEqual([1, 2, 3]);

    function nestedArrows(x) {
        return (() => (() => arguments[0])())();
    }
    expect(nestedArrows(99)).toBe(99);

    function arrowWritesMappedArguments(x) {
        return (() => {
            arguments[0] = 100;
            return x;
        })();
    }
    expect(arrowWritesMappedArguments(1)).toBe(100);
});
