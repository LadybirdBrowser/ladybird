test("basic eval() functionality", () => {
    expect(eval("1 + 2")).toBe(3);

    function foo(a) {
        var x = 5;
        eval("x += a");
        return x;
    }
    expect(foo(7)).toBe(12);
});

test("returns value of last value-producing statement", () => {
    // See https://tc39.es/ecma262/#sec-block-runtime-semantics-evaluation
    expect(eval("")).toBeUndefined();
    expect(eval("1;;;;;")).toBe(1);
    expect(eval("1;{}")).toBe(1);
    expect(eval("1;var a;")).toBe(1);
});

test("completion value of a labelled break", () => {
    // A `break label;` carries the last value V of the labelled statement (UpdateEmpty), and an
    // empty completion becomes undefined. The completion register must be initialized so that the
    // break path never exposes a stale register.
    expect(eval("L: { break L; }")).toBeUndefined();
    expect(eval("L: { 1; break L; 2; }")).toBe(1);
    expect(eval("L: { 1; break L; }")).toBe(1);
    expect(eval("x: { 42; break x; 99; }")).toBe(42);
});

test("a for-in iterator does not leak through a labelled break", () => {
    // Regression test: the internal for-in property iterator must never surface as a script value.
    // Previously a labelled break could return it as the eval completion value.
    const leaked = eval(`
        { function pad() {}; for (let key in { property: 1 }) break; }
        L: { if (this) break L; []; }
    `);
    expect(leaked).toBeUndefined();
});

test("syntax error", () => {
    expect(() => {
        eval("{");
    }).toThrowWithMessage(SyntaxError, "Unexpected token Eof. Expected CurlyClose (line: 1, column: 2)");
});

test("returns 1st argument unless 1st argument is a string", () => {
    var stringObject = new String("1 + 2");
    expect(eval(stringObject)).toBe(stringObject);
});

// These eval scope tests use function expressions due to bug #8198
var testValue = "outer";
test("eval only touches locals if direct use", function () {
    var testValue = "inner";
    expect(globalThis.eval("testValue")).toEqual("outer");
});

test("alias to eval works as a global eval", function () {
    var testValue = "inner";
    var eval1 = globalThis.eval;
    expect(eval1("testValue")).toEqual("outer");
});

test("eval evaluates all args", function () {
    var i = 0;
    expect(eval("testValue", i++, i++, i++)).toEqual("outer");
    expect(i).toEqual(3);
});

test("eval tests for exceptions", function () {
    var i = 0;
    expect(function () {
        eval("testValue", i++, i++, j, i++);
    }).toThrowWithMessage(ReferenceError, "'j' is not defined");
    expect(i).toEqual(2);
});

test("direct eval inherits non-strict evaluation", function () {
    expect(eval("01")).toEqual(1);
});

test("direct eval inherits strict evaluation", function () {
    "use strict";
    expect(() => {
        eval("01");
    }).toThrowWithMessage(SyntaxError, "Unprefixed octal number not allowed in strict mode");
});

test("global eval evaluates as non-strict", function () {
    "use strict";
    expect(globalThis.eval("01"));
});

test("indirect eval can be called multiple times", function () {
    function f() {
        return (0, eval)("1 + 2");
    }
    expect(f()).toBe(3);
    expect(f()).toBe(3);
});

test("indirect eval with syntax error can be called multiple times", function () {
    function f() {
        (0, eval)("@@@");
    }
    expect(f).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected statement or declaration (line: 1, column: 1)"
    );
    expect(f).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected statement or declaration (line: 1, column: 1)"
    );
});
