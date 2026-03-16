const DEEP_NESTING_DEPTH = 100000;

function makeDeepObjectBindingPattern(depth) {
    let pattern = "x";
    for (let i = 0; i < depth; ++i) pattern = `{a:${pattern}}`;
    return pattern;
}

test("deep object binding pattern nesting does not crash parser", () => {
    const source = `let ${makeDeepObjectBindingPattern(DEEP_NESTING_DEPTH)} = 0;`;
    expect(typeof canParseSource(source)).toBe("boolean");
});
