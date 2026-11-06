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

    // Regular function with duplicate parameters before rest parameter
    expect(`
        function foo(bar, bar, ...baz) {}
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

    // Duplicate across destructuring parameters
    expect(`
        function foo({ bar }, { bar }) {}
    `).not.toEval();

    // Duplicate between identifier and destructuring parameter
    expect(`
        function foo(bar, { bar }) {}
    `).not.toEval();

    // Duplicate between destructuring and identifier parameter
    expect(`
        function foo({ bar }, bar) {}
    `).not.toEval();

    // Object method with duplicate parameters
    expect(`
        ({ foo(bar, bar) {} });
    `).not.toEval();

    // Object method with rest parameter
    expect(`
        ({ foo(bar, ...bar) {} });
    `).not.toEval();
});

test("sloppy generators and async functions with a simple parameter list may repeat a parameter name", () => {
    expect("function* g(a, a) {}").toEval();
    expect("async function f(a, a) {}").toEval();
    expect("async function* h(a, a) {}").toEval();
    expect("(function* (a, a) {})").toEval();
    expect("(async function (a, a) {})").toEval();
    expect("(async function* (a, a) {})").toEval();

    function* generator(a, a) {
        yield a;
    }
    expect(generator(1, 2).next().value).toBe(2);
});

test("other generators and async functions may not repeat a parameter name", () => {
    expect("'use strict'; function* g(a, a) {}").not.toEval();
    expect("async function f(a, a) { 'use strict'; }").not.toEval();
    expect("function* g(a, [a]) {}").not.toEval();
    expect("async function f(a, a = 1) {}").not.toEval();
    expect("async (a, a) => {}").not.toEval();
    expect("({ *m(a, a) {} })").not.toEval();
    expect("({ async m(a, a) {} })").not.toEval();
});
