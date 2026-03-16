const DEEP_NESTING_DEPTH = 100000;

function makeDeepArrayBindingPattern(depth) {
    let pattern = "x";
    for (let i = 0; i < depth; ++i) pattern = `[${pattern}]`;
    return pattern;
}

test("deep array binding pattern nesting does not crash parser", () => {
    const source = `let ${makeDeepArrayBindingPattern(DEEP_NESTING_DEPTH)} = 0;`;
    expect(typeof canParseSource(source)).toBe("boolean");
});
