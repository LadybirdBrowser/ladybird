eval("var foo;");
evaluateSource("let foo = 1;");

test("redeclaration of variable in global scope succeeded", () => {
    expect(foo).toBe(1);
});
