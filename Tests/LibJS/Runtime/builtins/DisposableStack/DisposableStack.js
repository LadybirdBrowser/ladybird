test("constructor properties", () => {
    expect(DisposableStack).toHaveLength(0);
    expect(DisposableStack.name).toBe("DisposableStack");
});

describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            DisposableStack();
        }).toThrowWithMessage(TypeError, "DisposableStack constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new DisposableStack()).toBe("object");
    });
});
