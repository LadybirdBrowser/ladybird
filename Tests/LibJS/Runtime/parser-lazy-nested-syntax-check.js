test("deeply nested lazy functions still syntax-check inner function bodies", () => {
    const source = "function a(){ function b(){ function c(){ var x = ; } } }";

    expect(source).not.toEval();
});
