test("NewExpression is never a valid assignment target", () => {
    expect("new f() = 1").not.toEval();
    expect("new f() += 1").not.toEval();
    expect("new f()++").not.toEval();
    expect("++new f()").not.toEval();
    expect("for (new f() in x) {}").not.toEval();
    expect("for (new f() of x) {}").not.toEval();
});

test("CallExpression is not a valid assignment target in strict mode", () => {
    expect("'use strict'; f() = 1").not.toEval();
    expect("'use strict'; f() += 1").not.toEval();
    expect("'use strict'; f()++").not.toEval();
    expect("'use strict'; ++f()").not.toEval();
    expect("'use strict'; for (f() in x) {}").not.toEval();
    expect("'use strict'; for (f() of x) {}").not.toEval();
});

test("CallExpression is accepted as assignment target in non-strict mode (web compat)", () => {
    expect("f() = 1").toEval();
    expect("f() += 1").toEval();
    expect("f()++").toEval();
    expect("++f()").toEval();
    expect("for (f() in x) {}").toEval();
    expect("for (f() of x) {}").toEval();
});

test("Parenthesized object/array literals are not valid destructuring targets", () => {
    expect("({}) = 1").not.toEval();
    expect("([]) = 1").not.toEval();
    expect("({a: b}) = {a: 1}").not.toEval();
    expect("([a]) = [1]").not.toEval();
});

test("Non-parenthesized destructuring assignment still works", () => {
    expect("var a, b; ({a: b} = {a: 1})").toEval();
    expect("var a; [a] = [1]").toEval();
});
