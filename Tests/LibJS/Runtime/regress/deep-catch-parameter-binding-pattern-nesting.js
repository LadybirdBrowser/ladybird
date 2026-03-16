const DEEP_NESTING_DEPTH = 100000;

function makeDeepArrayBindingPattern(depth) {
    let pattern = "x";
    for (let i = 0; i < depth; ++i) pattern = `[${pattern}]`;
    return pattern;
}

test("deep catch parameter binding pattern nesting does not crash parser", () => {
    const source = `try {} catch (${makeDeepArrayBindingPattern(DEEP_NESTING_DEPTH)}) {}`;
    expect(typeof canParseSource(source)).toBe("boolean");
});
