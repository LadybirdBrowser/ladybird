test("length is 0", () => {
    expect(AsyncDisposableStack.prototype.move).toHaveLength(0);
});

describe("basic functionality", () => {
    test("stack is disposed after moving", () => {
        const stack = new AsyncDisposableStack();

        const newStack = stack.move();

        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeFalse();
    });

    test("move does not dispose resource but only move them", async () => {
        const stack = new AsyncDisposableStack();
        let disposeCalled = false;
        stack.defer(() => {
            disposeCalled = true;
        });

        expect(disposeCalled).toBeFalse();
        expect(stack.disposed).toBeFalse();

        const newStack = stack.move();

        expect(disposeCalled).toBeFalse();
        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeFalse();

        await stack.disposeAsync();

        expect(disposeCalled).toBeFalse();
        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeFalse();

        await newStack.disposeAsync();

        expect(disposeCalled).toBeTrue();
        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeTrue();
    });

    test("can add stack to itself", async () => {
        const stack = new AsyncDisposableStack();
        stack.move(stack);
        await stack.disposeAsync();
    });
});

describe("throws errors", () => {
    test("move throws if stack is already disposed (over type errors)", async () => {
        const stack = new AsyncDisposableStack();
        await stack.disposeAsync();
        expect(stack.disposed).toBeTrue();

        expect(() => stack.move()).toThrowWithMessage(ReferenceError, "AsyncDisposableStack already disposed values");
    });
});
