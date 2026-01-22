test("is getter without setter", () => {
    const property = Object.getOwnPropertyDescriptor(AsyncDisposableStack.prototype, "disposed");
    expect(property.get).not.toBeUndefined();
    expect(property.set).toBeUndefined();
    expect(property.value).toBeUndefined();
});

describe("basic functionality", () => {
    test("is not a property on the object itself", () => {
        const stack = new AsyncDisposableStack();
        expect(Object.hasOwn(stack, "disposed")).toBeFalse();
    });

    test("starts off as false", () => {
        const stack = new AsyncDisposableStack();
        expect(stack.disposed).toBeFalse();
    });

    test("becomes true after being disposed", async () => {
        const stack = new AsyncDisposableStack();
        await stack.disposeAsync();
        expect(stack.disposed).toBeTrue();
    });
});
