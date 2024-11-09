test("function with duplicate parameter names", () => {
    function foo(bar, _, bar) {
        return bar;
    }
    expect(foo(1, 2, 3)).toBe(3);
});

test("syntax errors", () => {
    // Regular function in strict mode
    expect(`
        "use strict";
        function foo(bar, bar) {}
    `).not.toEval();

    // Arrow function in strict mode
    expect(`
        "use strict";
        const foo = (bar, bar) => {};
    `).not.toEval();

    // Arrow function in non-strict mode
    expect(`
        const foo = (bar, bar) => {};
    `).not.toEval();

    // Regular function with rest parameter
    expect(`
        function foo(bar, ...bar) {}
    `).not.toEval();

    // Arrow function with rest parameter
    expect(`
        const foo = (bar, ...bar) => {};
    `).not.toEval();

    // Regular function with default parameter
    expect(`
        function foo(bar, bar = 1) {}
    `).not.toEval();

    // Arrow function with default parameter
    expect(`
        const foo = (bar, bar = 1) => {};
    `).not.toEval();
});
