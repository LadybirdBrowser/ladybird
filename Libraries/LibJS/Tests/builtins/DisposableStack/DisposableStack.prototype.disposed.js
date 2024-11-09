test("is getter without setter", () => {
    const property = Object.getOwnPropertyDescriptor(DisposableStack.prototype, "disposed");
    expect(property.get).not.toBeUndefined();
    expect(property.set).toBeUndefined();
    expect(property.value).toBeUndefined();
});

describe("basic functionality", () => {
    test("is not a property on the object itself", () => {
        const stack = new DisposableStack();
        expect(Object.hasOwn(stack, "disposed")).toBeFalse();
    });

    test("starts off as false", () => {
        const stack = new DisposableStack();
        expect(stack.disposed).toBeFalse();
    });

    test("becomes true after being disposed", () => {
        const stack = new DisposableStack();
        stack.dispose();
        expect(stack.disposed).toBeTrue();
    });
});
