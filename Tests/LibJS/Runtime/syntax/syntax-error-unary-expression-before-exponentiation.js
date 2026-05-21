test("syntax error for an unary expression before exponentiation", () => {
    expect(`!5 ** 2`).not.toEval();
    expect(`~5 ** 2`).not.toEval();
    expect(`+5 ** 2`).not.toEval();
    expect(`-5 ** 2`).not.toEval();
    expect(`typeof 5 ** 2`).not.toEval();
    expect(`void 5 ** 2`).not.toEval();
    expect(`delete 5 ** 2`).not.toEval();

    const AsyncFunction = async function () {}.constructor;
    expect(() => AsyncFunction("await 5 ** 2")).toThrow(SyntaxError);
    expect(() => AsyncFunction("(await 5) ** 2")).not.toThrow();
});
