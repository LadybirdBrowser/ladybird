const DEEP_NESTING_DEPTH = 100000;

test("deep conditional expression nesting does not crash parser", () => {
    let source = "0";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source = `(true ? ${source} : 0)`;

    expect(typeof canParseSource(source)).toBe("boolean");
});
