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

test("syntax error", () => {
    expect(() => {
        eval("{");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Eof. Expected CurlyClose (line: 1, column: 2)"
    );
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
