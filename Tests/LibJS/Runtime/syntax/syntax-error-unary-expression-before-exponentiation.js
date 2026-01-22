test("syntax error for an unary expression before exponentiation", () => {
    expect(`!5 ** 2`).not.toEval();
    expect(`~5 ** 2`).not.toEval();
    expect(`+5 ** 2`).not.toEval();
    expect(`-5 ** 2`).not.toEval();
    expect(`typeof 5 ** 2`).not.toEval();
    expect(`void 5 ** 2`).not.toEval();
    expect(`delete 5 ** 2`).not.toEval();
});
