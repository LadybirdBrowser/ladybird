test("Declaration in single-statement context is a syntax error", () => {
    expect("if (0) const foo = 1").not.toEval();
    expect("while (0) function foo() {}").not.toEval();
    expect("for (var 0;;) class foo() {}").not.toEval();
    expect("do let foo = 1 while (0)").not.toEval();
});
