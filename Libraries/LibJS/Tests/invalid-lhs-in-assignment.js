test("assignment to function call", () => {
    expect('function foo() {}; foo() = "foo";').not.toEval();
});

test("Postfix operator after function call", () => {
    expect('function foo() {}; foo()++;').not.toEval();
});

test("assignment to function call in strict mode", () => {
    expect("'use strict'; foo() = 'foo';").not.toEval();
});

test("assignment to inline function call", () => {
    expect('(function () {})() = "foo";').not.toEval();
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

test("assignment to call LHS is syntax error", () => {
    expect("f() += 1").not.toEval();
    expect("f() -= 1").not.toEval();
    expect("f() *= 1").not.toEval();
    expect("f() /= 1").not.toEval();
    expect("f() %= 1").not.toEval();
    expect("f() **= 1").not.toEval();
    expect("f() &= 1").not.toEval();
    expect("f() |= 1").not.toEval();
    expect("f() ^= 1").not.toEval();
    expect("f() <<= 1").not.toEval();
    expect("f() >>= 1").not.toEval();
    expect("f() >>>= 1").not.toEval();
    expect("f() = 1").not.toEval();
    expect("f() &&= 1").not.toEval();
    expect("f() ||= 1").not.toEval();
    expect("f() ??= 1").not.toEval();
});
