const DEEP_NESTING_DEPTH = 100000;

test("deep grouping expression nesting does not crash parser", () => {
    let source = "";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source += "(";
    source += "0";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source += ")";

    expect(typeof canParseSource(source)).toBe("boolean");
});
