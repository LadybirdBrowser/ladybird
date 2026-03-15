const DEEP_NESTING_DEPTH = 100000;

test("deep call expression nesting does not crash parser", () => {
    let source = "0";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source = `f(${source})`;

    expect(typeof canParseSource(source)).toBe("boolean");
});
