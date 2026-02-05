describe("parsing new expressions with optional chaining", () => {
    expect("new Object()?.foo").toEval();
    expect("new Object?.foo").not.toEval();
    expect("(new Object)?.foo").toEval();
});
