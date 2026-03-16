const DEEP_NESTING_DEPTH = 5000;

function makeDeepGroupingExpression(depth) {
    let value = "0";
    for (let i = 0; i < depth; ++i) value = `(${value})`;
    return value;
}

test("deep speculative arrow rollback preserves recursion-limit error", () => {
    // `(` triggers speculative arrow parsing first. This source is *not* an
    // arrow function, so parser state is rolled back via load_state().
    //
    // Without preserving recursion-limit diagnostics across rollback, the
    // overflow error from the speculative branch can get truncated.
    const source = `(a = ${makeDeepGroupingExpression(DEEP_NESTING_DEPTH)});`;
    expect(() => {
        evaluateSource(source);
    }).toThrowWithMessage(SyntaxError, "Maximum parser recursion depth exceeded");
});
