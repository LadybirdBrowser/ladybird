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
    function parses(depth) {
        try {
            (0, eval)(`(/${"(".repeat(depth)}x${")".repeat(depth)}/)`);
            return true;
        } catch {
            return false;
        }
    }
    // The search tries the depth right above the deepest one that parses, which has the least stack to spare.
    let low = 1;
    let high = 1 << 16;
    while (low + 1 < high) {
        const middle = (low + high) >> 1;
        if (parses(middle)) low = middle;
        else high = middle;
    }
    expect(parses(low)).toBeTrue();
});
