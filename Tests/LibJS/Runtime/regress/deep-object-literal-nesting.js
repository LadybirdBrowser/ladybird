const DEEP_NESTING_DEPTH = 100000;

test("deep object literal nesting does not crash parser", () => {
    let source = "0";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source = `({ a: ${source} })`;

    expect(typeof canParseSource(source)).toBe("boolean");
});
