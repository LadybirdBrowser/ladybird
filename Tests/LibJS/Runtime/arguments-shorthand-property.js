// Regression: shorthand `{ arguments }` and `{ eval }` inside an object
// literal IS a real reference to the binding, so the function needs to
// materialize the arguments object. Previously the parser never marked
// the function (the eval/arguments check was suppressed for property
// keys), so the local was allocated but uninitialized and reading the
// property crashed.

test("{ arguments } shorthand returns the arguments object", () => {
    function f() {
        return { arguments };
    }
    const r = f(1, 2, 3);
    expect(typeof r.arguments).toBe("object");
    expect(r.arguments.length).toBe(3);
    expect(r.arguments[0]).toBe(1);
    expect(r.arguments[2]).toBe(3);
});

test("{ arguments } shorthand inside a method", () => {
    const obj = {
        m() {
            return { arguments };
        },
    };
    const r = obj.m("a", "b");
    expect(r.arguments.length).toBe(2);
    expect(r.arguments[0]).toBe("a");
});

test("destructuring assignment { eval } = ... does not require arguments object", () => {
    // ({ eval } = { eval: 1 }) is a destructuring assignment to the
    // global eval, not a real `eval` reference inside the function.
    // It should parse and run without crashing.
    function fn() {
        ({ eval } = { eval: 1 });
        return 42;
    }
    expect(fn()).toBe(42);
});
