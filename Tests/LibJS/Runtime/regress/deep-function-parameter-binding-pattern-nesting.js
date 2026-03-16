const DEEP_NESTING_DEPTH = 100000;

function makeDeepObjectBindingPattern(depth) {
    let pattern = "x";
    for (let i = 0; i < depth; ++i) pattern = `{a:${pattern}}`;
    return pattern;
}

test("deep function parameter binding pattern nesting does not crash parser", () => {
    const source = `function f(${makeDeepObjectBindingPattern(DEEP_NESTING_DEPTH)}) {}`;
    expect(typeof canParseSource(source)).toBe("boolean");
});
