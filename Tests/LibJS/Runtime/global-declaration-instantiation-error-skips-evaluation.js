test("script body is not evaluated when GlobalDeclarationInstantiation fails", () => {
    evaluateSource("let x = 1;");
    expect(() => {
        evaluateSource("let x = 2; globalThis.__side_effect = true;");
    }).toThrowWithMessage(SyntaxError, "Redeclaration of top level variable");
    expect(globalThis.__side_effect).toBeUndefined();
});
