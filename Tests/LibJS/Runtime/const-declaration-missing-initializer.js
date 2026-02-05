test("missing initializer in 'const' variable declaration is syntax error", () => {
    expect("const foo").not.toEval();
    expect("const foo = 1, bar").not.toEval();
    expect("const foo = 1, bar, baz = 2").not.toEval();
});
