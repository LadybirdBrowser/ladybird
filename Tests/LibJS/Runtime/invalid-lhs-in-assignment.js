test.xfail("assignment to function call", () => {
    expect(() => {
        function foo() {}
        foo() = "foo";
    }).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
});

test.xfail("Postfix operator after function call", () => {
    expect(() => {
        function foo() {}
        foo()++;
    }).toThrow(ReferenceError);
});

test("assignment to function call in strict mode", () => {
    expect("'use strict'; foo() = 'foo'").toEval();
});

test.xfail("assignment to inline function call", () => {
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
