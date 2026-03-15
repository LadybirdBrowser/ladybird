// Syntax errors inside function bodies must be reported as early errors
// (at parse time), even if the function is never called.
// https://tc39.es/ecma262/#sec-static-semantics-early-errors
//
// NOTE: We use eval() here because test-js evaluates each test file as a
// script. eval() also parses as a script and must report early errors.
// If lazy parsing skips syntax checking, these tests will fail.

describe("syntax errors in inner functions are early errors", () => {
    test("syntax error in nested function declaration", () => {
        expect("function outer() { function inner() { var x = ; } }").not.toEval();
    });

    test("syntax error in nested function expression", () => {
        expect("function outer() { var f = function() { return = ; }; }").not.toEval();
    });

    test("syntax error in arrow function body", () => {
        expect("function outer() { var f = () => { var x = ; }; }").not.toEval();
    });

    test("syntax error in method definition", () => {
        expect("function outer() { var o = { m() { var x = ; } }; }").not.toEval();
    });

    test("syntax error in getter", () => {
        expect("function outer() { var o = { get x() { var y = ; } }; }").not.toEval();
    });

    test("syntax error in deeply nested function", () => {
        expect("function a() { function b() { function c() { var x = ; } } }").not.toEval();
    });

    test("syntax error in function inside class method", () => {
        expect("class C { m() { function f() { var x = ; } } }").not.toEval();
    });

    test("strict mode 'use strict' inside lazily-parsed function", () => {
        expect("function outer(){ function inner(){ 'use strict'; with({}){} } }").not.toEval();
    });

    test("inherited strict mode in lazily-parsed function", () => {
        expect("'use strict'; function outer(){ function inner(){ with({}){} } }").not.toEval();
    });

    test("strict mode in deeply nested lazy function does not leak to outer function", () => {
        expect("function outer(){ function b(){ function c(){ 'use strict'; } with({}){} } }").toEval();
    });

    test("shorthand property captures free variable from outer scope", () => {
        expect(`
            function outer(obj) {
                function inner() { return { source }; }
                const { source } = obj;
                return inner();
            }
            var r = outer({ source: 42 });
            if (r.source !== 42) throw new Error("FAIL");
        `).toEval();
    });

    test("syntax error at depth 4", () => {
        expect("function a(){ function b(){ function c(){ function d(){ var x = ; } } } }").not.toEval();
    });
});
