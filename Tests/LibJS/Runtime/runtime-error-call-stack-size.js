test("infinite recursion", () => {
    function infiniteRecursion() {
        infiniteRecursion();
    }

    try {
        infiniteRecursion();
    } catch (e) {
        expect(e).toBeInstanceOf(InternalError);
        expect(e.name).toBe("InternalError");
        expect(e.message).toBe("Call stack size limit exceeded");
    }

    expect(() => {
        JSON.stringify({}, () => ({ foo: "bar" }));
    }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");

    expect(() => {
        new Proxy({}, { get: (_, __, p) => p.foo }).foo;
    }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");

    expect(() => {
        function outer() {
            async function inner() {
                await outer;
            }
            inner();
            outer();
        }

        outer();
    }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
});

test("async-from-sync iterators created at the call stack size limit", () => {
    // Every frame of the recursion starts a for-await loop over an array, the deepest ones with almost no stack left,
    // and its async function catches the overflow in its rejected promise. See #12217.
    const recurse = async () => {
        try {
            recurse();
        } catch {}
        for await (const _ of [0]);
    };
    expect(() => {
        recurse();
        gc();
    }).not.toThrow();
});
