const DEEP_NESTING_DEPTH = 100000;

test("deep function nesting does not crash parser", () => {
    let source = "";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source += `function f${i}(){`;
    source += "0;";
    for (let i = 0; i < DEEP_NESTING_DEPTH; ++i) source += "}";

    expect(typeof canParseSource(source)).toBe("boolean");
});
