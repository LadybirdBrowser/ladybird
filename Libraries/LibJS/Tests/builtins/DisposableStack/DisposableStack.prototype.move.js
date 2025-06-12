test("length is 0", () => {
    expect(DisposableStack.prototype.move).toHaveLength(0);
});

describe("basic functionality", () => {
    test("stack is disposed after moving", () => {
        const stack = new DisposableStack();

        const newStack = stack.move();

        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeFalse();
    });

    test("move does not dispose resource but only move them", () => {
        const stack = new DisposableStack();
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

        stack.dispose();

        expect(disposeCalled).toBeFalse();
        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeFalse();

        newStack.dispose();

        expect(disposeCalled).toBeTrue();
        expect(stack.disposed).toBeTrue();
        expect(newStack.disposed).toBeTrue();
    });

    test("can add stack to itself", () => {
        const stack = new DisposableStack();
        stack.move(stack);
        stack.dispose();
    });
});

describe("throws errors", () => {
    test("move throws if stack is already disposed (over type errors)", () => {
        const stack = new DisposableStack();
        stack.dispose();
        expect(stack.disposed).toBeTrue();

        expect(() => stack.move()).toThrowWithMessage(
            ReferenceError,
            "DisposableStack already disposed values"
        );
    });
});
