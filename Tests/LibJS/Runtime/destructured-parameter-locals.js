test("array destructured parameter produces correct locals", () => {
    function f([x, y]) {
        return x + y;
    }
    expect(f([10, 20])).toBe(30);
});

test("object destructured parameter produces correct locals", () => {
    function f({ a, b }) {
        return a * b;
    }
    expect(f({ a: 3, b: 7 })).toBe(21);
});

test("mixed plain and destructured parameters", () => {
    function f(first, { x }, ...rest) {
        return first + x + rest.length;
    }
    expect(f(100, { x: 20 }, "a", "b")).toBe(122);
});

test("nested destructured parameters", () => {
    function f({ a: { b } }) {
        return b;
    }
    expect(f({ a: { b: 42 } })).toBe(42);
});

test("destructured parameter with default value", () => {
    function f({ x = 10 } = {}) {
        return x;
    }
    expect(f()).toBe(10);
    expect(f({ x: 5 })).toBe(5);
});

test("destructured parameter with aliased name", () => {
    function f({ a: renamed }) {
        return renamed;
    }
    expect(f({ a: 99 })).toBe(99);
});

test("array destructured with rest element", () => {
    function f([first, ...rest]) {
        return first + rest.length;
    }
    expect(f([10, 20, 30])).toBe(12);
});

test("multiple destructured parameters", () => {
    function f({ a }, [b, c]) {
        return a + b + c;
    }
    expect(f({ a: 1 }, [2, 3])).toBe(6);
});
