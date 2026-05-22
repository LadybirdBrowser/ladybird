test("assignment to function call", () => {
    expect(() => {
        function foo() {}
        foo() = "foo";
    }).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
});

test("Postfix operator after function call", () => {
    expect(() => {
        function foo() {}
        foo()++;
    }).toThrow(ReferenceError);
});

test("assignment to function call in strict mode is a SyntaxError", () => {
    expect("'use strict'; foo() = 'foo'").not.toEval();
});

test("strict assignment to unresolvable reference preserves initial reference", () => {
    delete globalThis.__test_unresolvable_assignment;
    expect(() => {
        "use strict";
        __test_unresolvable_assignment = (globalThis.__test_unresolvable_assignment = 5);
    }).toThrowWithMessage(ReferenceError, "'__test_unresolvable_assignment' is not defined");
    expect(globalThis.__test_unresolvable_assignment).toBe(5);
    delete globalThis.__test_unresolvable_assignment;
});

test("assignment to inline function call", () => {
    expect(() => {
        (function () {})() = "foo";
    }).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
});

test("assignment to invalid LHS is syntax error", () => {
    expect("1 += 1").not.toEval();
    expect("1 -= 1").not.toEval();
    expect("1 *= 1").not.toEval();
    expect("1 /= 1").not.toEval();
    expect("1 %= 1").not.toEval();
    expect("1 **= 1").not.toEval();
    expect("1 &= 1").not.toEval();
    expect("1 |= 1").not.toEval();
    expect("1 ^= 1").not.toEval();
    expect("1 <<= 1").not.toEval();
    expect("1 >>= 1").not.toEval();
    expect("1 >>>= 1").not.toEval();
    expect("1 = 1").not.toEval();
    expect("1 &&= 1").not.toEval();
    expect("1 ||= 1").not.toEval();
    expect("1 ??= 1").not.toEval();
});

test("assignment to call LHS is only syntax error for new operators", () => {
    expect("f() += 1").toEval();
    expect("f() -= 1").toEval();
    expect("f() *= 1").toEval();
    expect("f() /= 1").toEval();
    expect("f() %= 1").toEval();
    expect("f() **= 1").toEval();
    expect("f() &= 1").toEval();
    expect("f() |= 1").toEval();
    expect("f() ^= 1").toEval();
    expect("f() <<= 1").toEval();
    expect("f() >>= 1").toEval();
    expect("f() >>>= 1").toEval();
    expect("f() = 1").toEval();

    expect("f() &&= 1").not.toEval();
    expect("f() ||= 1").not.toEval();
    expect("f() ??= 1").not.toEval();
});
