test("constructor properties", () => {
    expect(AsyncDisposableStack).toHaveLength(0);
    expect(AsyncDisposableStack.name).toBe("AsyncDisposableStack");
});

describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            AsyncDisposableStack();
        }).toThrowWithMessage(TypeError, "AsyncDisposableStack constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new AsyncDisposableStack()).toBe("object");
    });
});
