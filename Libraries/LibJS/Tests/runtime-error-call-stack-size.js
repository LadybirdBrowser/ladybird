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
});
