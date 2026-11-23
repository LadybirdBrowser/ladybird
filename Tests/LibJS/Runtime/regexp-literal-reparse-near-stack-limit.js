test("regular expression literals reparsed near the call stack size limit", () => {
    // How much stack is left when a reparse runs depends on the frames above it, so this tries several patterns.
    const patterns = [100, 200, 300].map(depth => "(".repeat(depth) + "x" + ")".repeat(depth));
    function recurse() {
        for (const pattern of patterns) {
            try {
                eval(`(/${pattern}/)`);
            } catch {}
        }
        recurse();
    }
    expect(recurse).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
});

test("regular expression literals at the deepest nesting that the stack allows", () => {
    // The search tries the depth right above the deepest one that parses, which has the least stack to spare.
    // NB: How deep a literal parses depends on the frames above the eval, so each depth is tried only once.
    let low = 0;
    let high = 1 << 20;
    let errorAtHigh;
    while (low + 1 < high) {
        const middle = (low + high) >> 1;
        try {
            (0, eval)(`(/${"(".repeat(middle)}x${")".repeat(middle)}/)`);
            low = middle;
        } catch (error) {
            high = middle;
            errorAtHigh = error;
        }
    }
    expect(low).toBeGreaterThan(0);
    expect(errorAtHigh).toBeInstanceOf(SyntaxError);
    expect(errorAtHigh.message.includes("Maximum parser recursion depth exceeded")).toBeTrue();
});
