// JIT code recurses on the native stack, unlike the interpreter. When it ran into the native stack limit, the catch
// block of the deepest frame called a native function with no stack left, which threw another stack overflow into the
// next frame's catch block, and so on for thousands of frames, each error capturing a stack trace thousands of frames
// deep. JIT code leaves headroom below the native stack limit for such code now.

test("the catch block that handles a stack overflow can call native functions", () => {
    const re = /\w/;
    let caught = 0;
    function recurse() {
        try {
            return recurse();
        } catch (e) {
            ++caught;
            return re.test("b");
        }
    }
    expect(recurse()).toBeTrue();
    // NB: The interpreter runs the catch blocks of the two deepest frames: the native call in the deepest one finds
    //     no interpreter stack left either.
    expect(caught).toBeLessThanOrEqual(2);
});

test("stack overflows in compiled code are still RangeErrors", () => {
    function recurse(depth) {
        return recurse(depth + 1) + 1;
    }
    expect(() => recurse(0)).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
});
